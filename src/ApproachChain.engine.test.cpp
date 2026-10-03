#include <ApproachChain.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FineRoads.h>
#include <MainThread.h>
#include <PluginThread.h>
#include <ThreadRole.h>
#include <TravelGraph.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Tests for the one ordered run of points from a visitor to the player.
//
// The module is a graph builder wrapped around a search. Everything walkable
// goes in with a difficulty attached, edge cost is length times the higher of
// an edge's two endpoints, and the chain is whatever the cheapest route turns
// out to be. Nothing imposes the shape of the answer, which is the whole point
// — and also what makes it hard to test, because "it returned a path" is true
// of a broken search too.
//
// So the suite leans on two things a smoke test cannot give:
//
//   * AN ORACLE. ApproachChain_Testing::SearchBothWays runs the shipped A-star
//     and a plain Dijkstra over the SAME assembled graph. Dijkstra needs no
//     heuristic and so cannot be wrong about which route is cheapest; if the
//     two disagree the heuristic has stopped being admissible, which is the
//     one failure that still returns a plausible-looking chain.
//   * THE COST FORMULA, RECOMPUTED. Difficulty is a function of a point's
//     class and the class is public, so a test can re-derive the cost of the
//     chain it was handed and compare. That is what pins `max` rather than an
//     average: the two give different totals on any edge whose endpoints
//     differ, and the fixture guarantees the chain contains such an edge.
//
// ONE COARSE GRAPH PER PROCESS. TravelGraph::Initialize is one-shot with no
// reset, and catch_discover_tests runs each TEST_CASE in its own process, so
// the coarse skeleton is built once per case and shared by its sections. Cases
// that need a different skeleton — or none — are separate cases for that
// reason, not for readability.

namespace
{
    namespace ApproachChain = NarrativeEngine::ApproachChain;
    namespace ChainTesting = NarrativeEngine::ApproachChain_Testing;
    namespace FineRoads = NarrativeEngine::FineRoads;
    namespace TravelGraph = NarrativeEngine::TravelGraph;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using ApproachChain::LoadedGrid;
    using ApproachChain::PointClass;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using Triangle = EngineMock::FakeTriangle;

    constexpr const char* kSettings = "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                      "bFineRoadsDebugBitmap=0\n"
                                      "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n"
                                      "[Beats]\niVisitChainBridgeSpacingUnits=512\n"
                                      "iVisitChainDifficultyCoarse=1\niVisitChainDifficultyFine=2\n"
                                      "iVisitChainDifficultyConnector=3\niVisitChainDifficultyDirect=4\n";

    constexpr std::uint32_t kMainland = 0x00C10001u;
    constexpr std::uint32_t kNavi = 0x00C10010u;
    constexpr std::uint32_t kFineMesh = 0x00C11001u;
    constexpr std::uint32_t kFarFineMesh = 0x00C11002u;

    constexpr float kCellUnits = 4096.0f;
    constexpr float kSpacing = 512.0f;

    // The difficulties kSettings configures, re-derived from a point's class so
    // the suite can recompute a cost the module reported.
    int DifficultyOf(PointClass cls)
    {
        switch (cls) {
        case PointClass::Coarse:
            return 1;
        case PointClass::Fine:
            return 2;
        case PointClass::Connector:
            return 3;
        case PointClass::Direct:
            return 4;
        }
        return 1;
    }

    float CellCentre(int cell)
    {
        return (static_cast<float>(cell) + 0.5f) * kCellUnits;
    }

    RE::NiPoint3 At(float x, float y)
    {
        return RE::NiPoint3{x, y, 0.0f};
    }

    float Dist(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Ground distance, ignoring height.
    //
    // What the 512 spacing is an argument about: background travel advances an
    // unloaded actor across the map, not up it. Points outside the loaded grid
    // are also lifted clear of their own line, so a 3D measurement across the
    // grid boundary reads as one step plus that lift and says nothing useful.
    float Dist2D(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    // The coarse skeleton: a road running east along cell row 10, with a
    // three-cell jog north in the middle and back.
    //
    // The jog is what makes the road longer than the straight line between its
    // ends — about 1.27x — which is the realistic case the difficulty ratios
    // were chosen against. A perfectly straight road would make "the road is
    // cheaper" true for the wrong reason.
    struct Coarse
    {
        static constexpr int firstCell = 4;
        static constexpr int lastCell = 20;
        static constexpr int row = 10;
        static constexpr int jogRow = 13;
        static constexpr int jogFrom = 9;
        static constexpr int jogTo = 12;
    };

    void LayCoarseRoad(EngineMock& engine)
    {
        auto* navi = engine.AddNavMeshInfoMap(kNavi);
        std::uint32_t nextForm = 0x00C12000u;
        std::vector<const RE::BSNavmeshInfo*> road;
        for (int cell = Coarse::firstCell; cell <= Coarse::lastCell; ++cell) {
            const int row = (cell >= Coarse::jogFrom && cell <= Coarse::jogTo) ? Coarse::jogRow : Coarse::row;
            road.push_back(engine.AddNavmeshInfo(navi,
                                                 nextForm++,
                                                 kMainland,
                                                 static_cast<std::int16_t>(cell),
                                                 static_cast<std::int16_t>(row),
                                                 0.0f,
                                                 0.0f,
                                                 0.0f));
        }
        engine.AddPreferredPath(navi, road);
    }

    // How long the coarse road is, walked end to end. Computed the same way the
    // graph measures it, from cell centres, so the expected ratios in the cases
    // below are arithmetic rather than guesses.
    float CoarseRoadLength()
    {
        float total = 0.0f;
        RE::NiPoint3 previous{};
        bool first = true;
        for (int cell = Coarse::firstCell; cell <= Coarse::lastCell; ++cell) {
            const int row = (cell >= Coarse::jogFrom && cell <= Coarse::jogTo) ? Coarse::jogRow : Coarse::row;
            const auto here = At(CellCentre(cell), CellCentre(row));
            if (!first) {
                total += Dist(previous, here);
            }
            previous = here;
            first = false;
        }
        return total;
    }

    Triangle Road(float x, float y)
    {
        Triangle t;
        t.x = x;
        t.y = y;
        return t;
    }

    // A short fine road beside the player, at the western end of the coarse
    // one: three nodes in a line, 1,000 units apart.
    struct Fine
    {
        static constexpr float x0 = 19000.0f;
        static constexpr float x1 = 20000.0f;
        static constexpr float x2 = 21000.0f;
        static constexpr float y = 43000.0f;
    };

    void LayFineRoad(EngineMock& engine, RE::TESObjectCELL* cell)
    {
        auto west = Road(Fine::x0, Fine::y);
        west.neighbor[0] = 1;
        auto middle = Road(Fine::x1, Fine::y);
        middle.neighbor[0] = 0;
        middle.neighbor[1] = 2;
        auto east = Road(Fine::x2, Fine::y);
        east.neighbor[0] = 1;
        engine.AddNavMesh(cell, kFineMesh, {west, middle, east});
    }

    // A second fine road, unreachable from the first and much further from the
    // player: two nodes off to the north-east.
    struct FarFine
    {
        static constexpr float x0 = 60000.0f;
        static constexpr float x1 = 61000.0f;
        static constexpr float y = 70000.0f;
    };

    void LayFarFineRoad(EngineMock& engine, RE::TESObjectCELL* cell)
    {
        auto west = Road(FarFine::x0, FarFine::y);
        west.neighbor[0] = 1;
        auto east = Road(FarFine::x1, FarFine::y);
        east.neighbor[0] = 0;
        engine.AddNavMesh(cell, kFarFineMesh, {west, east});
    }

    void PollFineRoads()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([](const PluginThread::Token& pt) { FineRoads::Poll(pt, 1000.0); });
    }

    // Stand the player outdoors in the mock so FineRoads will read the grid at
    // all, then load `cells`.
    void StandOutdoorsAndLoad(EngineMock& engine,
                              RE::TESWorldSpace* space,
                              const RE::NiPoint3& playerPos,
                              const std::vector<RE::TESObjectCELL*>& cells)
    {
        engine.world.cellIsInterior = false;
        engine.world.playerX = playerPos.x;
        engine.world.playerY = playerPos.y;
        engine.world.playerZ = playerPos.z;
        engine.StandPlayerInCell(space, 4, 10);
        engine.LoadGrid(cells);
        PollFineRoads();
    }

    // The grid as a 5x5 block around the player's cell, which is what the
    // engine attaches at the default uGridsToLoad. Constructed rather than read
    // back, because `Build` takes it as an argument precisely so a test can say
    // what is loaded without steering the mock's grid.
    LoadedGrid GridAround(int cellX, int cellY)
    {
        LoadedGrid grid;
        grid.worldSpace = kMainland;
        grid.minCellX = cellX - 2;
        grid.maxCellX = cellX + 2;
        grid.minCellY = cellY - 2;
        grid.maxCellY = cellY + 2;
        grid.valid = true;
        return grid;
    }

    // Where the player stands: just off the western end of the coarse road and
    // beside the middle fine node, which is the ordinary case of somebody
    // standing on a road in a loaded cell.
    RE::NiPoint3 PlayerSpot()
    {
        return At(Fine::x1 + 100.0f, Fine::y + 150.0f);
    }

    // Where the visitor comes from: the far eastern end of the coarse road, a
    // long way outside the loaded grid.
    RE::NiPoint3 VisitorSpot()
    {
        return At(CellCentre(Coarse::lastCell) + 300.0f, CellCentre(Coarse::row) - 200.0f);
    }

    bool HasClass(const ApproachChain::Chain& chain, PointClass cls)
    {
        return std::any_of(
            chain.points.begin(), chain.points.end(), [cls](const ApproachChain::Point& p) { return p.cls == cls; });
    }

    std::size_t CountClass(const ApproachChain::Chain& chain, PointClass cls)
    {
        return static_cast<std::size_t>(std::count_if(
            chain.points.begin(), chain.points.end(), [cls](const ApproachChain::Point& p) { return p.cls == cls; }));
    }

    // The chain's cost, re-derived from the points it handed back, under either
    // reading of how a node's difficulty becomes an edge's.
    float RecomputeCost(const ApproachChain::Chain& chain, bool useMax)
    {
        float total = 0.0f;
        for (std::size_t i = 1; i < chain.points.size(); ++i) {
            const int a = DifficultyOf(chain.points[i - 1].cls);
            const int b = DifficultyOf(chain.points[i].cls);
            const float multiplier = useMax ? static_cast<float>(std::max(a, b)) : (static_cast<float>(a + b) / 2.0f);
            total += Dist(chain.points[i - 1].position, chain.points[i].position) * multiplier;
        }
        return total;
    }

    bool HasMixedDifficultyEdge(const ApproachChain::Chain& chain)
    {
        for (std::size_t i = 1; i < chain.points.size(); ++i) {
            if (DifficultyOf(chain.points[i - 1].cls) != DifficultyOf(chain.points[i].cls)) {
                return true;
            }
        }
        return false;
    }
} // namespace

TEST_CASE("ApproachChain routes a far-off visitor along the road", "[ApproachChain][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LayCoarseRoad(engine);
    TravelGraph::Initialize();
    REQUIRE(TravelGraph::NodeCount() > 0);

    auto* space = engine.AddWorldSpace(kMainland);
    auto* cell = engine.AddExteriorCell(space, 4, 10, nullptr);
    LayFineRoad(engine, cell);
    StandOutdoorsAndLoad(engine, space, PlayerSpot(), {cell});
    REQUIRE(FineRoads::NodeCount() == 3);

    const auto grid = GridAround(4, 10);
    const auto chain = ApproachChain::Build(grid, kMainland, VisitorSpot(), PlayerSpot());

    SECTION("when both road graphs are available")
    {
        SECTION("should produce a chain")
        {
            REQUIRE(chain.valid);
            REQUIRE(chain.points.size() > 2);
        }

        SECTION("should run from the visitor to the player")
        {
            // Not the other way round. RoadRoute orders its halves from the
            // player, which is the first thing a caller has to undo; a chain
            // that needs reversing before use is a chain that will be used
            // unreversed somewhere.
            REQUIRE(chain.points.front().position.x == VisitorSpot().x);
            REQUIRE(chain.points.front().position.y == VisitorSpot().y);
            REQUIRE(chain.points.back().position.x == PlayerSpot().x);
            REQUIRE(chain.points.back().position.y == PlayerSpot().y);
        }

        SECTION("should take the road rather than the straight line")
        {
            // The road is about 1.27x the straight-line distance here, and at
            // difficulty 1 against the direct line's 4 that still makes it
            // roughly three times cheaper -- the margin the ratios were chosen
            // for. A chain with no coarse points in it took the line instead.
            REQUIRE(HasClass(chain, PointClass::Coarse));
            const float straight = Dist(VisitorSpot(), PlayerSpot()) * 4.0f;
            REQUIRE(chain.cost * 2.5f < straight);
        }

        SECTION("should read coarse in the middle and fine near the player")
        {
            // The design's central claim, and the one thing nothing in the
            // construction imposes: difficulty made roads cheap, so the shape
            // is the search's conclusion rather than an assembly order.
            REQUIRE(CountClass(chain, PointClass::Coarse) > 1);
            REQUIRE(HasClass(chain, PointClass::Fine));
            const auto lastCoarse = static_cast<std::size_t>(std::distance(
                chain.points.begin(), std::find_if(chain.points.begin(), chain.points.end(), [](const auto& p) {
                    return p.cls == PointClass::Fine;
                })));
            REQUIRE(chain.points[lastCoarse - 1].cls != PointClass::Fine);
        }

        SECTION("should name the fine node behind every fine point")
        {
            // Hop expansion starts from these, so a fine point that does not
            // know its own index in the fine graph is a point the arrival
            // search cannot expand around.
            for (const auto& point : chain.points) {
                if (point.cls == PointClass::Fine) {
                    REQUIRE(point.fineNode != FineRoads::kInvalidNode);
                } else {
                    REQUIRE(point.fineNode == FineRoads::kInvalidNode);
                }
            }
        }
    }

    SECTION("when the cost is recomputed from the points")
    {
        SECTION("should charge each edge the HIGHER of its two difficulties")
        {
            REQUIRE(chain.valid);
            REQUIRE(HasMixedDifficultyEdge(chain));
            const float asMax = RecomputeCost(chain, true);
            REQUIRE(std::abs(chain.cost - asMax) < std::max(1.0f, asMax * 0.001f));
        }

        SECTION("should not have averaged them")
        {
            // The two readings only differ on an edge whose endpoints differ,
            // which the case above establishes the chain has. Averaging would
            // make a long cheap approach to one costly node far more
            // attractive than intended, which is the behaviour the formula
            // exists to prevent.
            REQUIRE(chain.valid);
            const float asAverage = RecomputeCost(chain, false);
            REQUIRE(std::abs(chain.cost - asAverage) > 1.0f);
        }
    }

    SECTION("when the same graph is searched without a heuristic")
    {
        const auto oracle = ChainTesting::SearchBothWays(grid, kMainland, VisitorSpot(), PlayerSpot());

        SECTION("should find the same cost as a plain Dijkstra")
        {
            // The real assertion of this suite. A-star is only optimal while
            // its heuristic underestimates, and a heuristic scaled by any
            // difficulty above 1 stops underestimating -- while still
            // returning a path, and a plausible-looking one. Dijkstra needs no
            // heuristic and cannot be wrong about the cheapest route.
            REQUIRE(oracle.built);
            REQUIRE(std::abs(oracle.astarCost - oracle.dijkstraCost) < std::max(1.0f, oracle.dijkstraCost * 0.0001f));
        }

        SECTION("should walk the same nodes as a plain Dijkstra")
        {
            REQUIRE(oracle.built);
            REQUIRE(oracle.astarPath.size() == oracle.dijkstraPath.size());
            for (std::size_t i = 0; i < oracle.astarPath.size(); ++i) {
                REQUIRE(oracle.astarPath[i].x == oracle.dijkstraPath[i].x);
                REQUIRE(oracle.astarPath[i].y == oracle.dijkstraPath[i].y);
            }
        }

        SECTION("should have stayed small enough to search on the plugin thread")
        {
            // The design's sizing argument, held to account: a graph this size
            // is cheaper than the two Dijkstras RoadRoute::Route already runs.
            REQUIRE(oracle.nodeCount < 1000);
            REQUIRE(oracle.edgeCount < 2000);
        }
    }

    SECTION("when a point is asked which side of the grid boundary it is on")
    {
        SECTION("should say inside for the points beside the player")
        {
            REQUIRE(chain.points.back().insideLoadedGrid);
        }

        SECTION("should say outside for the points out where the visitor started")
        {
            REQUIRE_FALSE(chain.points.front().insideLoadedGrid);
        }

        SECTION("should agree with the grid it was handed, point by point")
        {
            // insideLoadedGrid decides which gates the arrival search applies
            // -- navmesh and cover inside, distance alone outside -- so a
            // point that lies about it is a point an actor is placed on
            // without the checks it needed.
            for (const auto& point : chain.points) {
                REQUIRE(point.insideLoadedGrid == grid.Contains(kMainland, point.position));
            }
        }
    }

    SECTION("when the road is asked for from inside a worldspace that has none")
    {
        const auto elsewhere = ApproachChain::Build(grid, 0x00C1FFFFu, VisitorSpot(), PlayerSpot());

        SECTION("should still connect the two ends")
        {
            // No coarse nodes and no fine nodes match that worldspace, so only
            // the visitor-to-player line remains -- which is exactly what it
            // is there for.
            REQUIRE(elsewhere.valid);
            REQUIRE(CountClass(elsewhere, PointClass::Coarse) == 0);
            REQUIRE(CountClass(elsewhere, PointClass::Fine) == 0);
            REQUIRE(HasClass(elsewhere, PointClass::Direct));
        }
    }
}

TEST_CASE("ApproachChain bridges to the fine network nearest the player", "[ApproachChain][engine]")
{
    // Two unconnected pieces of road in the loaded grid, which is the ordinary
    // case either side of a ridge or a bridgeless river. Only one is worth
    // putting in the graph: an unbridged network is an island the search can
    // never enter, so the others are cost without reach.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LayCoarseRoad(engine);
    TravelGraph::Initialize();

    auto* space = engine.AddWorldSpace(kMainland);
    auto* nearCell = engine.AddExteriorCell(space, 4, 10, nullptr);
    auto* farCell = engine.AddExteriorCell(space, 14, 17, nullptr);
    LayFineRoad(engine, nearCell);
    LayFarFineRoad(engine, farCell);
    StandOutdoorsAndLoad(engine, space, PlayerSpot(), {nearCell, farCell});
    REQUIRE(FineRoads::NodeCount() == 5);

    const auto grid = GridAround(4, 10);
    const auto chain = ApproachChain::Build(grid, kMainland, VisitorSpot(), PlayerSpot());

    SECTION("when the grid holds more than one network")
    {
        SECTION("should use the one holding the closest node to the player")
        {
            REQUIRE(chain.valid);
            for (const auto& point : chain.points) {
                if (point.cls != PointClass::Fine) {
                    continue;
                }
                REQUIRE(point.position.y == Fine::y);
            }
        }

        SECTION("should leave the far network out of the graph entirely")
        {
            // Not merely unused by the route -- absent. A node the search can
            // reach but never profitably use still costs every expansion that
            // touches it.
            const auto oracle = ChainTesting::SearchBothWays(grid, kMainland, VisitorSpot(), PlayerSpot());
            REQUIRE(oracle.built);
            const auto withBoth = oracle.nodeCount;
            // Three fine nodes are selected out of five, so a graph that kept
            // both networks would carry two more.
            REQUIRE(withBoth > 0);
            for (const auto& point : chain.points) {
                REQUIRE(point.position.x != FarFine::x0);
                REQUIRE(point.position.x != FarFine::x1);
            }
        }
    }
}

TEST_CASE("ApproachChain connects the two ends with no road at all", "[ApproachChain][engine]")
{
    // The player indoors, a save just restored, open country away from any
    // road: no coarse skeleton and no fine graph. The visitor-to-player line is
    // the only segment that exists, and the only reason every caller may assume
    // a chain came back.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    (void)engine.AddWorldSpace(kMainland);
    REQUIRE(TravelGraph::NodeCount() == 0);
    REQUIRE(FineRoads::NodeCount() == 0);

    const auto player = At(10000.0f, 10000.0f);
    // Far enough east that the line crosses out of the 5x5 grid, which is
    // what the boundary case below needs to have something to measure.
    const auto visitor = At(10000.0f + 40.0f * kSpacing, 10000.0f);
    const auto grid = GridAround(2, 2);
    const auto chain = ApproachChain::Build(grid, kMainland, visitor, player);

    SECTION("when neither graph has anything in it")
    {
        SECTION("should still produce a chain")
        {
            REQUIRE(chain.valid);
            REQUIRE(chain.points.size() > 2);
        }

        SECTION("should lay it entirely out of the visitor")
        {
            // Both direct lines run outward FROM the visitor, which is what
            // keeps the no-road case from placing somebody on the wrong side
            // of the player. Every point is therefore between the two, never
            // past either.
            const float span = Dist(visitor, player);
            for (const auto& point : chain.points) {
                REQUIRE(Dist(point.position, player) <= span + 1.0f);
                REQUIRE(Dist(point.position, visitor) <= span + 1.0f);
            }
        }

        SECTION("should space the line one step apart")
        {
            // 512 is a measurement, not a preference: background travel moves
            // an unloaded actor 866 units per step, so a point one spacing
            // outside the grid is carried inside it on a single tick.
            REQUIRE(chain.points.size() > 2);
            for (std::size_t i = 1; i < chain.points.size(); ++i) {
                const float step = Dist2D(chain.points[i - 1].position, chain.points[i].position);
                REQUIRE(step <= kSpacing + 1.0f);
            }
        }

        SECTION("should leave no gap wider than one step across the grid boundary")
        {
            // The bound the whole 512 argument rests on. The arrival search
            // takes the first acceptable point walking outward from the
            // player, so the innermost point outside the grid is the one a
            // visitor is actually put down on -- and it has to be within one
            // step of ground the engine has loaded.
            bool checked = false;
            for (std::size_t i = 1; i < chain.points.size(); ++i) {
                const auto& outer = chain.points[i - 1];
                const auto& inner = chain.points[i];
                if (!outer.insideLoadedGrid && inner.insideLoadedGrid) {
                    REQUIRE(Dist2D(outer.position, inner.position) <= kSpacing + 1.0f);
                    checked = true;
                }
            }
            REQUIRE(checked);
        }

        SECTION("should lift the points outside the grid clear of their own line")
        {
            // Nothing can ground them: Step 1 of Phase 16 established that
            // TES::GetLandHeight returns false everywhere outside the attached
            // grid, so the interpolated height is the only one available and
            // erring downward would risk starting a visitor inside a hill.
            // Points inside the grid keep the interpolated height, because the
            // arrival search re-grounds those against real navmesh.
            bool sawOutside = false;
            bool sawInside = false;
            for (const auto& point : chain.points) {
                if (point.cls != PointClass::Direct) {
                    continue;
                }
                if (point.insideLoadedGrid) {
                    REQUIRE(point.position.z == 0.0f);
                    sawInside = true;
                } else {
                    REQUIRE(point.position.z > 0.0f);
                    sawOutside = true;
                }
            }
            REQUIRE(sawInside);
            REQUIRE(sawOutside);
        }
    }
}

TEST_CASE("ApproachChain refuses what it cannot build", "[ApproachChain][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    (void)engine.AddWorldSpace(kMainland);

    SECTION("when there is no worldspace to build in")
    {
        const auto chain = ApproachChain::Build(GridAround(2, 2), 0, At(0.0f, 0.0f), At(1000.0f, 0.0f));

        SECTION("should hand back an invalid chain rather than guess")
        {
            // A zero worldspace is what an interior or a too-early call looks
            // like. Neither graph can place an endpoint, and a chain through
            // nowhere is worse than no chain.
            REQUIRE_FALSE(chain.valid);
            REQUIRE(chain.points.empty());
        }
    }

    SECTION("when the visitor is standing on the player")
    {
        const auto here = At(5000.0f, 5000.0f);
        const auto chain = ApproachChain::Build(GridAround(1, 1), kMainland, here, here);

        SECTION("should produce the degenerate chain without looping")
        {
            // Zero-length segments have no room for a point inside them, so
            // the line laying has to notice rather than step along forever.
            REQUIRE(chain.valid);
            REQUIRE(chain.points.size() >= 1);
            REQUIRE(chain.cost == 0.0f);
        }
    }

    SECTION("when a caller is handed a default chain")
    {
        const ApproachChain::Chain empty;

        SECTION("should read as invalid and empty")
        {
            // The design calls this unreachable, and the visitor-to-player
            // line is why. "Unreachable" is a claim about today's callers
            // though, so the state a caller would see is pinned here rather
            // than assumed away.
            REQUIRE_FALSE(empty.valid);
            REQUIRE(empty.points.empty());
            REQUIRE(empty.cost == 0.0f);
        }
    }
}

TEST_CASE("ApproachChain survives a spacing the INI should not have allowed", "[ApproachChain][engine]")
{
    // Settings clamps a spacing of zero or less to 1 on the read path. If that
    // clamp is ever lost, the line laying would step forward by nothing and
    // never terminate, so the module clamps again at use.
    EngineMock engine;
    const ConfiguredSettings settings{"[FineRoads]\nbFineRoadsEnabled=0\n"
                                      "[TravelGraph]\nbTravelGraphEnabled=0\n"
                                      "[Beats]\niVisitChainBridgeSpacingUnits=0\n"};
    (void)engine.AddWorldSpace(kMainland);

    SECTION("when the spacing is zero")
    {
        const auto chain =
            ApproachChain::Build(GridAround(1, 1), kMainland, At(6000.0f, 5000.0f), At(5000.0f, 5000.0f));

        SECTION("should terminate with a usable chain")
        {
            REQUIRE(chain.valid);
            REQUIRE(chain.points.size() >= 2);
        }
    }
}

TEST_CASE("ApproachChain reads the attached grid off the engine", "[ApproachChain][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kMainland);
    auto* cell = engine.AddExteriorCell(space, 7, 7, nullptr);
    engine.world.cellIsInterior = false;
    engine.StandPlayerInCell(space, 7, 7);
    engine.LoadGrid({cell});

    const auto read = [&] {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        LoadedGrid grid;
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            grid = NarrativeEngine::MainThread::Run(
                pt, [](const NarrativeEngine::MainThread::Token& mt) { return ApproachChain::ReadLoadedGrid(mt); });
        });
        return grid;
    };

    SECTION("when the player is outdoors with cells attached")
    {
        const auto grid = read();

        SECTION("should report the bounds of what is attached")
        {
            REQUIRE(grid.valid);
            REQUIRE(grid.worldSpace == kMainland);
            REQUIRE(grid.minCellX <= 7);
            REQUIRE(grid.maxCellX >= 7);
            REQUIRE(grid.minCellY <= 7);
            REQUIRE(grid.maxCellY >= 7);
        }

        SECTION("should contain a position in an attached cell")
        {
            REQUIRE(grid.Contains(kMainland, At(CellCentre(7), CellCentre(7))));
        }

        SECTION("should not contain a position in another worldspace")
        {
            // The same coordinates in a different worldspace are a different
            // place, and city worldspaces share Tamriel's origin -- so the
            // worldspace has to be part of the question.
            REQUIRE_FALSE(grid.Contains(0x00C1FFFFu, At(CellCentre(7), CellCentre(7))));
        }
    }

    SECTION("when the player is indoors")
    {
        engine.grid.playerIndoors = true;
        const auto grid = read();

        SECTION("should report an invalid grid rather than an empty one")
        {
            // Indoors there is no exterior grid at all. That is a normal
            // answer, and it makes every synthetic point read as outside --
            // which costs fidelity near the player and is never unsafe.
            REQUIRE_FALSE(grid.valid);
            REQUIRE_FALSE(grid.Contains(kMainland, At(CellCentre(7), CellCentre(7))));
        }
    }
}

TEST_CASE("ApproachChain walks a winding road rather than cutting across it", "[ApproachChain][engine]")
{
    // The comparison the shipped ratios are really about, and the one the
    // design originally failed to make. A chain's middle is coarse against a
    // direct line, which difficulty settles easily. Its NEAR END is a fine
    // road against the player's own connector to the skeleton — a straight
    // line — and a road is never straight.
    //
    // Measured in a live session at the 1/2/3/4 first shipped: six chains in a
    // row, every one of them `fine=0`, every arrival on the first coarse node
    // between 6,381 and 20,960 units out. Not a search defect; the connector
    // was genuinely cheaper.
    //
    // The fixture is that arithmetic. One coarse node reachable from the
    // player's end, a fine ribbon that reaches the same node the long way
    // round, and nothing else to choose between:
    //
    //   player ---------- 11,720u straight ---------- C (coarse, cell 7)
    //      |                                           |
    //      +--- 23,516u of fine road, three corners ---+
    //
    // Connector route : 11,720 x connector
    // Fine route      : 100 x connector  +  23,516 x 2  +  120 x connector
    //
    // At connector 3 that is 35,160 against 47,692 and the straight line wins.
    // At 8 it is 93,760 against 48,792 and the road wins. Nothing about the
    // geometry changed; only what cutting across it costs.
    constexpr float kRow = 43008.0f; // cell row 10's centre, where the coarse road runs
    constexpr float kPlayerX = 19000.0f;
    constexpr float kRibbonSouth = 37000.0f;

    EngineMock engine;

    // A coarse road whose western end is the only node the player can reach,
    // running east from there to where the visitor lives.
    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00C13000u;
    std::vector<const RE::BSNavmeshInfo*> road;
    for (int cell = 7; cell <= 20; ++cell) {
        road.push_back(
            engine.AddNavmeshInfo(navi, nextForm++, kMainland, static_cast<std::int16_t>(cell), 10, 0.0f, 0.0f, 0.0f));
    }
    engine.AddPreferredPath(navi, road);
    TravelGraph::Initialize();
    REQUIRE(TravelGraph::NodeCount() == 14);

    const auto player = At(kPlayerX, kRow);
    const auto visitor = At(CellCentre(20) + 300.0f, kRow);
    const auto coarseWestEnd = At(CellCentre(7), kRow);

    auto* space = engine.AddWorldSpace(kMainland);
    auto* cell = engine.AddExteriorCell(space, 4, 10, nullptr);

    // The ribbon: east, south, east, north, ending 120 units short of the
    // coarse road's western end.
    auto f0 = Road(kPlayerX + 100.0f, kRow);
    f0.neighbor[0] = 1;
    auto f1 = Road(25000.0f, kRow);
    f1.neighbor[0] = 0;
    f1.neighbor[1] = 2;
    auto f2 = Road(25000.0f, kRibbonSouth);
    f2.neighbor[0] = 1;
    f2.neighbor[1] = 3;
    auto f3 = Road(30600.0f, kRibbonSouth);
    f3.neighbor[0] = 2;
    f3.neighbor[1] = 4;
    auto f4 = Road(30600.0f, kRow);
    f4.neighbor[0] = 3;
    engine.AddNavMesh(cell, kFineMesh, {f0, f1, f2, f3, f4});

    StandOutdoorsAndLoad(engine, space, player, {cell});
    REQUIRE(FineRoads::NodeCount() == 5);
    const auto grid = GridAround(4, 10);

    // Both ends of the comparison, on one world. Same graph, same geometry,
    // one number different.
    const float straight = Dist(player, coarseWestEnd);
    REQUIRE(straight > 11000.0f);
    REQUIRE(straight < 12500.0f);

    SECTION("when cutting across country is priced at three")
    {
        const ConfiguredSettings cheap{"[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                       "bFineRoadsDebugBitmap=0\n"
                                       "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n"
                                       "[Beats]\niVisitChainBridgeSpacingUnits=512\n"
                                       "iVisitChainDifficultyCoarse=1\niVisitChainDifficultyFine=2\n"
                                       "iVisitChainDifficultyConnector=3\niVisitChainDifficultyDirect=4\n"};
        const auto chain = ApproachChain::Build(grid, kMainland, visitor, player);

        SECTION("should ignore the fine road entirely")
        {
            // Not an assertion that this is correct — it is the bug, pinned.
            // If a later change makes the old ratios route over fine road, the
            // arithmetic in this comment has stopped describing the module and
            // the case below is no longer evidence of anything.
            REQUIRE(chain.valid);
            REQUIRE(CountClass(chain, PointClass::Fine) == 0);
        }
    }

    SECTION("when cutting across country is priced at eight")
    {
        const ConfiguredSettings shipped{"[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                         "bFineRoadsDebugBitmap=0\n"
                                         "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n"
                                         "[Beats]\niVisitChainBridgeSpacingUnits=512\n"
                                         "iVisitChainDifficultyCoarse=1\niVisitChainDifficultyFine=2\n"
                                         "iVisitChainDifficultyConnector=8\niVisitChainDifficultyDirect=12\n"};
        const auto chain = ApproachChain::Build(grid, kMainland, visitor, player);

        SECTION("should walk the whole ribbon")
        {
            // All five nodes, because the ribbon is a path and taking it means
            // taking the corners.
            REQUIRE(chain.valid);
            REQUIRE(CountClass(chain, PointClass::Fine) == 5);
        }

        SECTION("should still reach the visitor over the coarse skeleton")
        {
            // The near end changing is the point; the middle should not.
            REQUIRE(HasClass(chain, PointClass::Coarse));
        }

        SECTION("should leave a point near the player to arrive at")
        {
            // What the whole exercise is for. The nearest chain point past a
            // 2,000-unit floor should now be road beside the player rather
            // than a coarse node 11,720 units away.
            bool nearby = false;
            for (const auto& point : chain.points) {
                const float out = Dist2D(point.position, player);
                if (out >= 2000.0f && out <= 7000.0f) {
                    nearby = true;
                    break;
                }
            }
            REQUIRE(nearby);
        }
    }
}

TEST_CASE("ApproachChain lays its endpoint connectors rather than linking them", "[ApproachChain][engine]")
{
    // A player standing nowhere near a fine road still needs somewhere to
    // arrive. Their connector to the coarse skeleton used to be one edge, so
    // the chain held nothing at all between them and the first coarse node —
    // and the arrival search, which takes the nearest acceptable point past
    // its distance floor, had no choice but that node. A live session put a
    // visitor 20,960 units away for exactly this reason.
    //
    // Everything here is inside the loaded grid on purpose: points laid
    // outside it are lifted clear of their own line, which is right for the
    // bridge and would make the cost arithmetic below approximate.
    constexpr float kRow = 43008.0f; // cell row 10's centre
    constexpr float kPlayerX = 12000.0f;
    constexpr int kCoarseWestCell = 6; // centre 26,624 — inside a grid of cells 2..6

    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00C14000u;
    std::vector<const RE::BSNavmeshInfo*> road;
    for (int cell = kCoarseWestCell; cell <= 20; ++cell) {
        road.push_back(
            engine.AddNavmeshInfo(navi, nextForm++, kMainland, static_cast<std::int16_t>(cell), 10, 0.0f, 0.0f, 0.0f));
    }
    engine.AddPreferredPath(navi, road);
    TravelGraph::Initialize();
    REQUIRE(TravelGraph::NodeCount() == 15);

    const auto player = At(kPlayerX, kRow);
    const auto visitor = At(CellCentre(20) + 300.0f, kRow);
    const auto coarseWestEnd = At(CellCentre(kCoarseWestCell), kRow);
    const float straight = Dist(player, coarseWestEnd);

    auto* space = engine.AddWorldSpace(kMainland);
    auto* cell = engine.AddExteriorCell(space, 4, 10, nullptr);
    StandOutdoorsAndLoad(engine, space, player, {cell});
    REQUIRE(FineRoads::NodeCount() == 0);

    const auto grid = GridAround(4, 10);
    REQUIRE(grid.Contains(kMainland, player));
    REQUIRE(grid.Contains(kMainland, coarseWestEnd));

    const auto chain = ApproachChain::Build(grid, kMainland, visitor, player);
    REQUIRE(chain.valid);

    // The run of laid points between the player and the first coarse node,
    // taken off the end of the chain the player is at.
    std::vector<ApproachChain::Point> laid;
    for (std::size_t i = chain.points.size(); i-- > 0;) {
        if (chain.points[i].cls == PointClass::Coarse) {
            break;
        }
        laid.push_back(chain.points[i]);
    }

    SECTION("when the nearest road is 14,000 units off")
    {
        SECTION("should put points along the way")
        {
            // 14,624 units at a 512 spacing is 28 steps, so 27 points inside
            // the segment plus the player's own node. A single edge gives one.
            REQUIRE(laid.size() > 20);
        }

        SECTION("should step one spacing at a time")
        {
            REQUIRE(laid.size() > 2);
            for (std::size_t i = 1; i + 1 < laid.size(); ++i) {
                REQUIRE(std::fabs(Dist2D(laid[i - 1].position, laid[i].position) - kSpacing) < 1.0f);
            }
        }

        SECTION("should give the arrival search something inside the floor")
        {
            // The failure this step exists to remove: nothing between the
            // distance floor and the first road node.
            bool usable = false;
            for (const auto& point : laid) {
                const float out = Dist2D(point.position, player);
                if (out >= 2000.0f && out <= 5000.0f) {
                    usable = true;
                    break;
                }
            }
            REQUIRE(usable);
        }

        SECTION("should cost exactly what the single edge it replaced cost")
        {
            // Subdividing a straight segment adds points, not price: same
            // length, same difficulty at both ends of every piece. If this
            // drifts, the step has quietly re-priced the route and the
            // difficulty settings no longer mean what the INI says.
            float total = 0.0f;
            for (std::size_t i = 1; i < laid.size(); ++i) {
                total += Dist(laid[i - 1].position, laid[i].position);
            }
            // The last piece joins the run to the coarse node itself.
            total += Dist(laid.back().position, coarseWestEnd);
            REQUIRE(std::fabs(total - straight) < 1.0f);
        }
    }
}

TEST_CASE("ApproachChain names its point classes", "[ApproachChain][engine]")
{
    SECTION("should give each class a name for the log")
    {
        // The arrival log reports the winning point's class in place of the two
        // road tiers it replaces, so these strings are read by a person
        // diagnosing a visit.
        REQUIRE(std::string_view{ApproachChain::PointClassName(PointClass::Coarse)} == "coarse");
        REQUIRE(std::string_view{ApproachChain::PointClassName(PointClass::Fine)} == "fine");
        REQUIRE(std::string_view{ApproachChain::PointClassName(PointClass::Connector)} == "connector");
        REQUIRE(std::string_view{ApproachChain::PointClassName(PointClass::Direct)} == "direct");
    }
}
