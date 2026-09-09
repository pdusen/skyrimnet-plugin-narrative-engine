#include <TravelGraph.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <HoldGrid.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// Tests for the long-distance road skeleton.
//
// Skyrim ships one NAVI record holding precomputed routes: ordered chains of
// navmeshes the engine walks a traveller along when they are too far away to
// simulate properly. Plotted end to end those chains are the road network, and
// this module turns them into a graph that can be queried.
//
// Reading it is awkward in a way that is worth pinning down. The chains are
// made of BSNavmeshInfo pointers, and CommonLibSSE only forward-declares that
// type — no members, no layout, nothing to read a position out of. Most
// navmeshes have no resident NavMesh form to ask instead, so almost every hop
// would be unplaceable and the graph would fall apart into islands.
//
// The module's answer is to measure the layout at startup rather than hardcode
// it: the minority of navmeshes that DO have a resident form give pairs of
// (opaque block, position we already trust), and the offset that reproduces
// those across every sample is taken as the position field. Three things then
// have to hold, and each is a case below. The measurement has to find the field
// when it is there. It has to find nothing rather than something when it is
// not. And a point that lands nowhere near the cell the engine filed it under
// has to be refused even after a successful measurement, because a
// plausible-looking offset that happens to hold other float data would
// otherwise scatter nodes across the map.
//
// The harness therefore lays out the opaque block itself, and a case moves or
// removes a field to ask what the measurement does about it.
//
// ONE GRAPH PER PROCESS. Initialize is one-shot with no reset, by design: the
// graph is const for the session once built. So a section cannot vary the world
// — whatever the first section builds is what every later section in the same
// test case sees. Each test case therefore builds its world once, above the
// sections, and the sections only ask it questions. This is also why the
// degenerate worlds each get a test case of their own. The suite is run through
// ctest, which gives every test case its own process; running the executable
// directly puts them all in one and the first graph built wins.

namespace
{
    namespace TravelGraph = NarrativeEngine::TravelGraph;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr const char* kSettings = "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    // Harness records, not vanilla ones: nothing here is looked up by editor ID
    // or FormID against the game's own data, so these are only keys.
    constexpr std::uint32_t kMainland = 0x00A00001u;
    constexpr std::uint32_t kIsland = 0x00A00002u;
    constexpr std::uint32_t kNavi = 0x00A00010u;

    constexpr std::uint32_t kNorthGate = 0x00A01001u;
    constexpr std::uint32_t kCrossroads = 0x00A01002u;
    constexpr std::uint32_t kMilestone = 0x00A01003u;
    constexpr std::uint32_t kSouthGate = 0x00A01004u;
    constexpr std::uint32_t kSideTrack = 0x00A01005u;
    constexpr std::uint32_t kMisfiled = 0x00A01006u;
    constexpr std::uint32_t kUntravelled = 0x00A01007u;
    constexpr std::uint32_t kIslandEast = 0x00A01008u;
    constexpr std::uint32_t kIslandWest = 0x00A01009u;

    constexpr float kCellUnits = 4096.0f;

    // Where the engine would put a navmesh filed under this cell if nothing
    // better were known — the fallback every unplaceable node lands on.
    float CellCentre(int cell)
    {
        return (static_cast<float>(cell) + 0.5f) * kCellUnits;
    }

    // The road. Two worldspaces, so that a query restricted to one is a real
    // restriction and a route between them is a real absence of one.
    //
    //   NorthGate — Crossroads — Milestone — SouthGate — Misfiled
    //                    |
    //                SideTrack
    //
    // Untravelled sits off on its own, known to the record and named by no
    // route. IslandEast and IslandWest are a separate road in another
    // worldspace.
    //
    // The positions the record holds sit a little off the centre of their cell,
    // which is what makes it possible to tell a node placed from the record
    // apart from one that fell back to the cell it was filed under.
    struct Road
    {
        float northGateX = 0.0f;
        float crossroadsX = 0.0f;
        float milestoneX = 0.0f;
        float southGateX = 0.0f;
        float sideTrackY = 0.0f;
        float y = 0.0f;
        float misfiledFallbackX = 0.0f;
    };

    Road BuildRoad(EngineMock& engine)
    {
        Road road;
        road.y = CellCentre(10) + 200.0f;
        road.northGateX = CellCentre(10) + 100.0f;
        road.crossroadsX = CellCentre(11) + 100.0f;
        road.milestoneX = CellCentre(12) + 100.0f;
        road.southGateX = CellCentre(13) + 100.0f;
        road.sideTrackY = CellCentre(11) + 200.0f;
        road.misfiledFallbackX = CellCentre(14);

        auto* navi = engine.AddNavMeshInfoMap(kNavi);

        const auto* northGate =
            engine.AddNavmeshInfo(navi, kNorthGate, kMainland, 10, 10, road.northGateX, road.y, 0.0f);
        const auto* crossroads =
            engine.AddNavmeshInfo(navi, kCrossroads, kMainland, 11, 10, road.crossroadsX, road.y, 0.0f);
        const auto* milestone =
            engine.AddNavmeshInfo(navi, kMilestone, kMainland, 12, 10, road.milestoneX, road.y, 0.0f);
        const auto* southGate =
            engine.AddNavmeshInfo(navi, kSouthGate, kMainland, 13, 10, road.southGateX, road.y, 0.0f);
        const auto* sideTrack =
            engine.AddNavmeshInfo(navi, kSideTrack, kMainland, 12, 11, road.milestoneX, road.sideTrackY, 0.0f);
        // Filed under cell 14 but claiming a point on the other side of the
        // province — the case for the sanity gate.
        const auto* misfiled = engine.AddNavmeshInfo(navi, kMisfiled, kMainland, 14, 10, 900000.0f, 900000.0f, 0.0f);
        (void)engine.AddNavmeshInfo(navi, kUntravelled, kMainland, 20, 20, CellCentre(20), CellCentre(20), 0.0f);
        const auto* islandEast =
            engine.AddNavmeshInfo(navi, kIslandEast, kIsland, 2, 2, CellCentre(2), CellCentre(2), 0.0f);
        const auto* islandWest =
            engine.AddNavmeshInfo(navi, kIslandWest, kIsland, 3, 2, CellCentre(3), CellCentre(2), 0.0f);

        // The two navmeshes with a resident form. These are the only ones whose
        // position is known from outside the opaque block, so they are the only
        // samples the layout can be measured against.
        auto* space = engine.AddWorldSpace(kMainland);
        auto* cell = engine.AddExteriorCell(space, 10, 10, nullptr);
        engine.AddNavMeshForm(
            kNorthGate, cell, road.northGateX - 1000.0f, road.y - 1000.0f, road.northGateX + 1000.0f, road.y + 1000.0f);
        engine.AddNavMeshForm(kCrossroads,
                              cell,
                              road.crossroadsX - 1000.0f,
                              road.y - 1000.0f,
                              road.crossroadsX + 1000.0f,
                              road.y + 1000.0f);

        // One long route down the road, and a second that retraces part of it:
        // chains overlap wherever two journeys share tarmac.
        engine.AddPreferredPath(navi, {northGate, crossroads, milestone, southGate, misfiled});
        engine.AddPreferredPath(navi, {crossroads, milestone});
        // A branch, which is what turns the milestone into a junction.
        engine.AddPreferredPath(navi, {milestone, sideTrack});
        // A route with a hop the record cannot place. The run has to break
        // there rather than join the two ends across it.
        engine.AddPreferredPath(navi, {northGate, nullptr, southGate});
        // The other worldspace.
        engine.AddPreferredPath(navi, {islandEast, islandWest});
        return road;
    }

    std::size_t NodeAt(float x, float y)
    {
        for (std::size_t i = 0; i < TravelGraph::NodeCount(); ++i) {
            const auto* node = TravelGraph::GetNode(i);
            if (node && node->x == x && node->y == y)
                return i;
        }
        return TravelGraph::kInvalidNode;
    }

    bool Placed(float x, float y)
    {
        return NodeAt(x, y) != TravelGraph::kInvalidNode;
    }
} // namespace

// HoldGrid's cell-to-hold partition, which the debug bitmap tints its
// background with. Defined here so the grid builder stays out of this link
// closure, and so a case can say what the map says without building one.
namespace NarrativeEngine::HoldGrid
{
    RE::FormID LookupCell(RE::TESWorldSpace*, std::int16_t cellX, std::int16_t)
    {
        // Two holds meeting at cell 12, so the bitmap has more than one colour
        // to lay down and the palette assignment runs.
        return cellX < 12 ? 0x00A0F001u : 0x00A0F002u;
    }
} // namespace NarrativeEngine::HoldGrid

TEST_CASE("TravelGraph builds a road from the precomputed routes", "[TravelGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const Road road = BuildRoad(engine);
    TravelGraph::Initialize();

    SECTION("when the routes have been walked")
    {
        SECTION("should keep a node for every navmesh a route passes through")
        {
            // Six of the eight mainland-and-island navmeshes are named by some
            // route; the seventh is not, and the eighth is on the island.
            REQUIRE(TravelGraph::NodeCount() == 8);
        }

        SECTION("should drop the navmeshes no route ever names")
        {
            // The record knows about every navmesh in the game and only a
            // fraction of them are road. Keeping the rest would make the
            // nearest-node query answer "nearest patch of ground".
            REQUIRE_FALSE(Placed(CellCentre(20), CellCentre(20)));
        }

        SECTION("should link each consecutive pair of hops")
        {
            REQUIRE(TravelGraph::EdgeCount() == 6);
        }

        SECTION("should count a shared stretch of road once")
        {
            // Two routes both run from the crossroads to the milestone. The
            // second must join the first at a junction rather than lay a
            // parallel corridor, or every cost summed over the graph doubles
            // wherever journeys happen to overlap.
            const auto crossroads = NodeAt(road.crossroadsX, road.y);
            const auto milestone = NodeAt(road.milestoneX, road.y);
            REQUIRE(TravelGraph::FindPath(crossroads, milestone).size() == 2);
        }

        SECTION("should not join two hops across one it could not place")
        {
            // The route names a navmesh the record has no entry for. Linking
            // across the gap would invent a road between the two ends of a
            // stretch nobody knows the shape of.
            const auto north = NodeAt(road.northGateX, road.y);
            const auto south = NodeAt(road.southGateX, road.y);
            REQUIRE(TravelGraph::FindPath(north, south).size() == 4);
        }
    }

    SECTION("when a node is placed")
    {
        SECTION("should use the point the record holds")
        {
            // Not the centre of the cell it was filed under. A cell is 4096
            // units across, so the difference between the two is the
            // difference between a road and a band of open ground.
            REQUIRE(Placed(road.northGateX, road.y));
        }

        SECTION("should recover the navmesh it belongs to")
        {
            // Found by the same measurement, and the reason to trust it: two
            // fields landing where they were predicted is far stronger evidence
            // that the right struct is being read than one field alone.
            const auto* node = TravelGraph::GetNode(NodeAt(road.northGateX, road.y));
            REQUIRE(node != nullptr);
            REQUIRE(node->navMesh == kNorthGate);
        }

        SECTION("should record the worldspace it sits in")
        {
            const auto* node = TravelGraph::GetNode(NodeAt(road.northGateX, road.y));
            REQUIRE(node != nullptr);
            REQUIRE(node->worldSpace == kMainland);
        }

        SECTION("should fall back to the cell when the point is nowhere near it")
        {
            // A point on the far side of the province from the cell it was
            // filed under is not a position, it is a misreading. The centre of
            // the cell is off by at most half a cell; a wild coordinate is off
            // by the width of Skyrim and would drag any route through it.
            REQUIRE(Placed(road.misfiledFallbackX, CellCentre(10)));
        }
    }

    SECTION("when an index outside the graph is asked for")
    {
        SECTION("should hand back nothing")
        {
            REQUIRE(TravelGraph::GetNode(TravelGraph::NodeCount()) == nullptr);
        }
    }

    SECTION("when the build is asked for a second time")
    {
        TravelGraph::Initialize();

        SECTION("should leave the graph alone")
        {
            // It runs at kDataLoaded and the graph is const for the session
            // afterwards. A second build would double every node.
            REQUIRE(TravelGraph::NodeCount() == 8);
        }
    }
}

TEST_CASE("TravelGraph finds the nearest node", "[TravelGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const Road road = BuildRoad(engine);
    TravelGraph::Initialize();

    SECTION("when a position on the road is given")
    {
        SECTION("should answer with the node beside it")
        {
            const auto found = TravelGraph::FindNearestNode(kMainland, road.crossroadsX + 50.0f, road.y);
            REQUIRE(found == NodeAt(road.crossroadsX, road.y));
        }
    }

    SECTION("when the position is nearer a node in another worldspace")
    {
        SECTION("should stay in the worldspace it was asked about")
        {
            // Worldspaces have their own coordinate systems, and two of them
            // overlap freely. A nearest-node query that crossed between them
            // would route a traveller in Skyrim towards a road on Solstheim.
            const auto found = TravelGraph::FindNearestNode(kIsland, road.northGateX, road.y);
            const auto* node = TravelGraph::GetNode(found);
            REQUIRE(node != nullptr);
            REQUIRE(node->worldSpace == kIsland);
        }
    }

    SECTION("when the worldspace has no road in it at all")
    {
        SECTION("should answer with nothing")
        {
            REQUIRE(TravelGraph::FindNearestNode(0x00A0FFFFu, road.northGateX, road.y) == TravelGraph::kInvalidNode);
        }
    }

    SECTION("when two nodes are equally far away")
    {
        SECTION("should still answer")
        {
            // Only that it answers, and always the same one: the query is used
            // to anchor a route, and an answer that varied run to run would
            // make routes vary with it.
            const float midpoint = (road.northGateX + road.crossroadsX) * 0.5f;
            const auto first = TravelGraph::FindNearestNode(kMainland, midpoint, road.y);
            REQUIRE(first != TravelGraph::kInvalidNode);
            REQUIRE(TravelGraph::FindNearestNode(kMainland, midpoint, road.y) == first);
        }
    }
}

TEST_CASE("TravelGraph routes between two nodes", "[TravelGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const Road road = BuildRoad(engine);
    TravelGraph::Initialize();

    const auto north = NodeAt(road.northGateX, road.y);
    const auto milestone = NodeAt(road.milestoneX, road.y);
    const auto sideTrack = NodeAt(road.milestoneX, road.sideTrackY);
    const auto island = TravelGraph::FindNearestNode(kIsland, CellCentre(2), CellCentre(2));

    SECTION("when the two are connected")
    {
        const auto path = TravelGraph::FindPath(north, sideTrack);

        SECTION("should start where it was asked to start")
        {
            REQUIRE_FALSE(path.empty());
            REQUIRE(path.front() == north);
        }

        SECTION("should end where it was asked to end")
        {
            REQUIRE_FALSE(path.empty());
            REQUIRE(path.back() == sideTrack);
        }

        SECTION("should go by the road rather than in a straight line")
        {
            // The branch to the side track leaves from the milestone. A route
            // that skipped it would be a route through country the graph never
            // said was passable.
            REQUIRE(path.size() == 4);
            REQUIRE(path[2] == milestone);
        }
    }

    SECTION("when the two are in separate road systems")
    {
        SECTION("should answer with no route")
        {
            // The graph is deliberately not connected: separate worldspaces and
            // separate road systems are separate components, and pretending
            // otherwise would hand back a path that crosses the sea.
            REQUIRE(TravelGraph::FindPath(north, island).empty());
        }
    }

    SECTION("when the start and the end are the same node")
    {
        SECTION("should answer with just that node")
        {
            REQUIRE(TravelGraph::FindPath(north, north) == std::vector<std::size_t>{north});
        }
    }

    SECTION("when one end is not a node")
    {
        SECTION("should answer with no route")
        {
            REQUIRE(TravelGraph::FindPath(north, TravelGraph::NodeCount()).empty());
            REQUIRE(TravelGraph::FindPath(TravelGraph::NodeCount(), north).empty());
        }
    }
}

TEST_CASE("TravelGraph measures the whole road at once", "[TravelGraph][engine]")
{
    // The distance field exists so a caller ranking many candidates against one
    // destination pays for one search rather than one per candidate.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const Road road = BuildRoad(engine);
    TravelGraph::Initialize();

    const auto north = NodeAt(road.northGateX, road.y);
    const auto crossroads = NodeAt(road.crossroadsX, road.y);
    const auto island = TravelGraph::FindNearestNode(kIsland, CellCentre(2), CellCentre(2));

    SECTION("when the field is taken from a node")
    {
        const auto field = TravelGraph::DistanceField(north);

        SECTION("should cover every node")
        {
            REQUIRE(field.size() == TravelGraph::NodeCount());
        }

        SECTION("should put the source at no distance from itself")
        {
            REQUIRE(field[north] == 0.0f);
        }

        SECTION("should measure a neighbour along the road")
        {
            // One cell apart, and the weight is the distance between node
            // centres rather than a hop count — a graph where every hop cost
            // one would rank a long detour equal to a short one.
            REQUIRE(std::fabs(field[crossroads] - (road.crossroadsX - road.northGateX)) < 1.0f);
        }

        SECTION("should leave an unreachable node at infinity")
        {
            REQUIRE(field[island] > 1.0e30f);
        }
    }

    SECTION("when the source is not a node")
    {
        SECTION("should answer with nothing")
        {
            REQUIRE(TravelGraph::DistanceField(TravelGraph::NodeCount()).empty());
        }
    }
}

TEST_CASE("TravelGraph falls back when the layout cannot be measured", "[TravelGraph][engine]")
{
    // The opaque block holds no position anywhere, which is what a runtime that
    // moved the field past the search window, or changed it entirely, would
    // look like. Degrading has to mean coarser positions, not wrong ones.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    engine.navInfo.writePosition = false;
    const Road road = BuildRoad(engine);
    TravelGraph::Initialize();

    SECTION("when a node is placed")
    {
        SECTION("should use the centre of the cell it was filed under")
        {
            REQUIRE(Placed(CellCentre(10), CellCentre(10)));
            REQUIRE_FALSE(Placed(road.northGateX, road.y));
        }

        SECTION("should still build the road")
        {
            // Half a cell of error in every node still leaves a usable
            // skeleton; refusing to build at all leaves nothing.
            REQUIRE(TravelGraph::NodeCount() == 8);
            REQUIRE(TravelGraph::EdgeCount() == 6);
        }

        SECTION("should claim no navmesh")
        {
            // The FormID is only looked for once a position has been found, so
            // a failed measurement leaves the node unattributed rather than
            // attributed on the strength of a field nobody located.
            const auto* node = TravelGraph::GetNode(NodeAt(CellCentre(10), CellCentre(10)));
            REQUIRE(node != nullptr);
            REQUIRE(node->navMesh == 0);
        }
    }
}

TEST_CASE("TravelGraph builds nothing without a record to read", "[TravelGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kMainland);
    (void)engine.AddExteriorCell(space, 10, 10, nullptr);
    TravelGraph::Initialize();

    SECTION("when the load order has no NAVI record")
    {
        SECTION("should give up rather than guess")
        {
            REQUIRE(TravelGraph::NodeCount() == 0);
        }
    }
}

TEST_CASE("TravelGraph builds nothing when the record knows no navmeshes", "[TravelGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    (void)engine.AddNavMeshInfoMap(kNavi);
    TravelGraph::Initialize();

    SECTION("when the record is there but empty")
    {
        SECTION("should give up")
        {
            // Which is what a save loaded before the record is populated looks
            // like, and what an install with the record stripped looks like.
            REQUIRE(TravelGraph::NodeCount() == 0);
        }
    }
}

TEST_CASE("TravelGraph builds nothing without the data handler", "[TravelGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    engine.world.dataHandlerPresent = false;
    TravelGraph::Initialize();

    SECTION("when the load order is not up yet")
    {
        SECTION("should give up")
        {
            REQUIRE(TravelGraph::NodeCount() == 0);
        }
    }
}

TEST_CASE("TravelGraph builds nothing while it is switched off", "[TravelGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[TravelGraph]\nbTravelGraphEnabled=0\n"};
    (void)BuildRoad(engine);
    TravelGraph::Initialize();

    SECTION("when the poll comes round")
    {
        SECTION("should not read the record at all")
        {
            // It is diagnostic and nothing consumes it yet, so the switch has
            // to mean no work rather than work nobody reads.
            REQUIRE(TravelGraph::NodeCount() == 0);
        }
    }
}

TEST_CASE("TravelGraph draws the road on request", "[TravelGraph][engine]")
{
    // The bitmap is the only way the skeleton has been checked against the game
    // world, which is what the module exists to make possible.
    const std::filesystem::path logDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};
    const auto mainland = logDir / ("NarrativeEngine_TravelGraph_" + std::string{"00A00001"} + ".bmp");
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    std::filesystem::remove(mainland, ec);

    EngineMock engine;
    const ConfiguredSettings settings{"[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=1\n"
                                      "iTravelGraphBitmapUnitsPerPixel=256\n"};
    (void)BuildRoad(engine);
    TravelGraph::Initialize();

    SECTION("when the dump is turned on")
    {
        SECTION("should write one image per worldspace")
        {
            // Named for the worldspace rather than numbered, because the whole
            // point is opening one and recognising the province in it.
            REQUIRE(std::filesystem::exists(mainland));
        }
    }
}
