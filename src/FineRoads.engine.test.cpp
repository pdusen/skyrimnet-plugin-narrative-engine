#include <FineRoads.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Tests for the fine road graph.
//
// The module reads Skyrim's navmesh and keeps the triangles Bethesda flagged as
// preferred — which is to say, the road surface. What comes out is a graph of
// where the roads actually run in the cells around the player, at a resolution
// the long-distance path skeleton does not have.
//
// Three things about it are worth pinning, and each is easy to get subtly wrong
// in a way nothing would report.
//
// The first is which triangles count. A mesh holds thousands and only a handful
// are road; including one that is not puts a node in open ground, and a route
// through it sends a traveller across country.
//
// The second is what an edge means. A triangle's three edge slots hold one of
// two different things depending on a flag: an index into the same mesh, or an
// index into a small table of portals across to another mesh. Reading one as
// the other silently invents adjacency between unrelated pieces of road. Both
// readings are bounds-checked, so the cost of a misreading is a missing link
// rather than a crash — and a missing link is exactly what a test has to catch.
//
// The third is the frontier. A node is a frontier when its road leaves the
// loaded region: it has a portal to a navmesh nobody has extracted. That is the
// handoff point to the coarse graph, and it is deliberately narrower than "near
// the edge of the grid". A flagged triangle whose neighbour is unflagged is the
// side of the road ribbon; a portal into a mesh we do have, with no road behind
// it, is a genuine dead end. Neither is a frontier, and calling either one a
// frontier would have the router hand off in the middle of a cell.
//
// The engine side is a fabricated cell grid whose navmeshes a test writes
// triangle by triangle — which is exactly the input the extractor reads.
//
// One thing shapes every case below: the module has no reset. Its per-cell
// cache is session-scoped on purpose, and there is no hook to clear it, so
// state carries from one section to the next within a process. Two habits keep
// that from mattering. Cells are built fresh in each section, so each gets a
// FormID the cache has never seen; and every poll is handed a huge elapsed
// time, so the backstop that decides whether to rescan is never the thing under
// test except in the one case that is about it.

namespace
{
    namespace FineRoads = NarrativeEngine::FineRoads;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using Triangle = EngineMock::FakeTriangle;

    // The debug bitmap defaults to ON, and writing one out of a poll that a test
    // runs dozens of times is both slow and a way for one case to change what the
    // case about the bitmap observes. Every case but that one turns it off.
    constexpr const char* kSettings = "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                      "bFineRoadsDebugBitmap=0\n";

    constexpr std::uint32_t kTamriel = 0x0000003Cu;
    constexpr std::uint32_t kMeshA = 0x00050001u;
    constexpr std::uint32_t kMeshB = 0x00050002u;
    constexpr std::uint32_t kUnloadedMesh = 0x00059999u;

    // Enough elapsed time to clear any backstop. Every case that is not about
    // the rescan schedule asks for a poll this way, so the schedule is never
    // an accidental reason a case passes or fails.
    constexpr double kPastTheBackstop = 1000.0;

    void Poll(double elapsedSeconds = kPastTheBackstop)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { FineRoads::Poll(pt, elapsedSeconds); });
    }

    // A road triangle at a position, with nothing across any of its edges.
    Triangle RoadAt(float x, float y)
    {
        Triangle t;
        t.x = x;
        t.y = y;
        t.z = 0.0f;
        return t;
    }

    // Nodes come out in the cache's iteration order, which is an unordered
    // map's, so a case names a node by where it is rather than by its index.
    std::size_t NodeAt(const FineRoads::Graph& graph, float x, float y)
    {
        for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].x == x && graph.nodes[i].y == y)
                return i;
        }
        return FineRoads::kInvalidNode;
    }

    bool Linked(const FineRoads::Graph& graph, std::size_t a, std::size_t b)
    {
        if (a >= graph.adjacency.size() || b >= graph.adjacency.size())
            return false;
        const auto& out = graph.adjacency[a];
        return std::find(out.begin(), out.end(), b) != out.end();
    }

    // Whether the two nodes at these positions know about each other. The link
    // is written from both ends, so a case that only checked one direction
    // would pass on a graph a router could only walk one way.
    bool LinkedBothWays(const FineRoads::Graph& graph, float ax, float ay, float bx, float by)
    {
        const auto a = NodeAt(graph, ax, ay);
        const auto b = NodeAt(graph, bx, by);
        if (a == FineRoads::kInvalidNode || b == FineRoads::kInvalidNode)
            return false;
        return Linked(graph, a, b) && Linked(graph, b, a);
    }

    bool IsFrontier(const FineRoads::Graph& graph, float x, float y)
    {
        const auto i = NodeAt(graph, x, y);
        return i != FineRoads::kInvalidNode && graph.nodes[i].frontier;
    }
} // namespace

TEST_CASE("FineRoads keeps only the road surface", "[FineRoads][engine]")
{
    // Happy path, re-run per leaf: one exterior cell, loaded and attached,
    // alone in the grid. What sits on its navmesh is what each case varies.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    engine.LoadGrid({cell});

    SECTION("when the cell carries a stretch of road")
    {
        engine.AddNavMesh(cell, kMeshA, {RoadAt(100.0f, 200.0f), RoadAt(300.0f, 400.0f)});
        Poll();

        SECTION("should turn each road triangle into a node")
        {
            REQUIRE(FineRoads::NodeCount() == 2);
        }

        SECTION("should put the node at the middle of the triangle")
        {
            // A router steers to these coordinates. A node at a vertex rather
            // than the centroid sits on the edge of the road surface, and the
            // error compounds along a route.
            REQUIRE(NodeAt(FineRoads::Snapshot(), 100.0f, 200.0f) != FineRoads::kInvalidNode);
        }
    }

    SECTION("when the mesh also holds triangles that are not road")
    {
        auto offRoad = RoadAt(300.0f, 400.0f);
        offRoad.preferred = false;
        engine.AddNavMesh(cell, kMeshA, {RoadAt(100.0f, 200.0f), offRoad});
        Poll();

        SECTION("should keep only the road")
        {
            // The overwhelming majority of a mesh is open ground. Taking it all
            // would bury the road in a field of nodes and route travellers
            // across country.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }

    SECTION("when a road triangle has been deleted")
    {
        auto removed = RoadAt(300.0f, 400.0f);
        removed.deleted = true;
        engine.AddNavMesh(cell, kMeshA, {RoadAt(100.0f, 200.0f), removed});
        Poll();

        SECTION("should leave it out")
        {
            // An edit that removed a piece of road leaves the triangle in the
            // record with a flag on it, still carrying its preferred flag.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }

    SECTION("when a road triangle names a vertex the mesh does not have")
    {
        engine.AddNavMeshWithBadVertexIndex(cell, kMeshA);
        Poll();

        SECTION("should skip it rather than read past the array")
        {
            REQUIRE(FineRoads::NodeCount() == 0);
        }
    }

    SECTION("when the cell has no navmesh at all")
    {
        Poll();

        SECTION("should contribute nothing")
        {
            // Ocean and border cells genuinely have none.
            REQUIRE(FineRoads::NodeCount() == 0);
        }
    }

    SECTION("when the cell has a navmesh list with nothing in it")
    {
        engine.AddEmptyNavMeshList(cell);
        Poll();

        SECTION("should contribute nothing")
        {
            REQUIRE(FineRoads::NodeCount() == 0);
        }
    }
}

TEST_CASE("FineRoads links triangles that share an edge", "[FineRoads][engine]")
{
    // Happy path, re-run per leaf: one cell, one mesh, and two road triangles
    // whose edge 0 faces the other.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    engine.LoadGrid({cell});

    auto first = RoadAt(100.0f, 0.0f);
    auto second = RoadAt(200.0f, 0.0f);

    SECTION("when two road triangles name each other")
    {
        first.neighbor[0] = 1;
        second.neighbor[0] = 0;
        engine.AddNavMesh(cell, kMeshA, {first, second});
        Poll();

        SECTION("should link them in both directions")
        {
            // A router walks the adjacency in whichever direction it is
            // travelling, so a one-way link is a road that can only be
            // followed one way.
            REQUIRE(LinkedBothWays(FineRoads::Snapshot(), 100.0f, 0.0f, 200.0f, 0.0f));
        }
    }

    SECTION("when only one of them names the other")
    {
        // Which is the ordinary case in the data: the record stores each
        // triangle's own view, and the graph has to complete the pair.
        first.neighbor[0] = 1;
        engine.AddNavMesh(cell, kMeshA, {first, second});
        Poll();

        SECTION("should still link them in both directions")
        {
            REQUIRE(LinkedBothWays(FineRoads::Snapshot(), 100.0f, 0.0f, 200.0f, 0.0f));
        }
    }

    SECTION("when the triangle across the edge is not road")
    {
        auto offRoad = RoadAt(200.0f, 0.0f);
        offRoad.preferred = false;
        first.neighbor[0] = 1;
        engine.AddNavMesh(cell, kMeshA, {first, offRoad});
        Poll();

        SECTION("should leave the node unlinked")
        {
            const auto graph = FineRoads::Snapshot();
            REQUIRE(graph.nodes.size() == 1);
            REQUIRE(graph.adjacency[0].empty());
        }

        SECTION("should not call it a frontier")
        {
            // This is the side of the road ribbon, not the end of coverage.
            // Marking it would have the router hand off to the coarse graph
            // every few yards along any road.
            REQUIRE_FALSE(IsFrontier(FineRoads::Snapshot(), 100.0f, 0.0f));
        }
    }

    SECTION("when an edge has nothing across it")
    {
        engine.AddNavMesh(cell, kMeshA, {first, second});
        Poll();

        SECTION("should leave both nodes unlinked")
        {
            const auto graph = FineRoads::Snapshot();
            REQUIRE(graph.adjacency[0].empty());
            REQUIRE(graph.adjacency[1].empty());
        }
    }

    SECTION("when a triangle names a neighbour the mesh does not have")
    {
        first.neighbor[0] = 40; // past the end of a two-triangle mesh
        engine.AddNavMesh(cell, kMeshA, {first, second});
        Poll();

        SECTION("should ignore the edge")
        {
            const auto graph = FineRoads::Snapshot();
            REQUIRE(graph.adjacency[0].empty());
        }
    }

    SECTION("when a triangle names itself")
    {
        first.neighbor[0] = 0;
        engine.AddNavMesh(cell, kMeshA, {first, second});
        Poll();

        SECTION("should not give it a link to itself")
        {
            // A self-link is a cycle of length one, which a route search walks
            // forever.
            const auto graph = FineRoads::Snapshot();
            const auto node = NodeAt(graph, 100.0f, 0.0f);
            REQUIRE(graph.adjacency[node].empty());
        }
    }

    SECTION("when two of a triangle's edges face the same neighbour")
    {
        first.neighbor[0] = 1;
        first.neighbor[1] = 1;
        second.neighbor[0] = 0;
        engine.AddNavMesh(cell, kMeshA, {first, second});
        Poll();

        SECTION("should record the link once")
        {
            // A duplicate makes the same road count twice in any cost the
            // router sums over adjacency.
            const auto graph = FineRoads::Snapshot();
            const auto node = NodeAt(graph, 100.0f, 0.0f);
            REQUIRE(graph.adjacency[node].size() == 1);
        }
    }
}

TEST_CASE("FineRoads links across a navmesh portal", "[FineRoads][engine]")
{
    // Happy path, re-run per leaf: one cell carrying two navmeshes, with a
    // road triangle in each facing the other across a portal. Meshes meet at
    // cell and quadrant boundaries, so this is what a road crossing one looks
    // like.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    engine.LoadGrid({cell});

    auto here = RoadAt(100.0f, 0.0f);
    auto there = RoadAt(200.0f, 0.0f);

    SECTION("when the road crosses into another loaded mesh")
    {
        here.portalMesh[0] = kMeshB;
        here.portalTriangle[0] = 0;
        engine.AddNavMesh(cell, kMeshA, {here});
        engine.AddNavMesh(cell, kMeshB, {there});
        Poll();

        SECTION("should link the two sides")
        {
            REQUIRE(LinkedBothWays(FineRoads::Snapshot(), 100.0f, 0.0f, 200.0f, 0.0f));
        }

        SECTION("should not call either side a frontier")
        {
            // Coverage does not end here — the mesh on the far side is loaded.
            REQUIRE_FALSE(IsFrontier(FineRoads::Snapshot(), 100.0f, 0.0f));
        }
    }

    SECTION("when the portal lands on a triangle that is not road")
    {
        there.preferred = false;
        here.portalMesh[0] = kMeshB;
        here.portalTriangle[0] = 0;
        engine.AddNavMesh(cell, kMeshA, {here});
        engine.AddNavMesh(cell, kMeshB, {there});
        Poll();

        SECTION("should leave the node unlinked")
        {
            const auto graph = FineRoads::Snapshot();
            REQUIRE(graph.nodes.size() == 1);
            REQUIRE(graph.adjacency[0].empty());
        }

        SECTION("should not call it a frontier")
        {
            // A genuine dead end: the mesh across the portal is loaded and has
            // no road on it. Handing off to the coarse graph here would send a
            // traveller onward from a road that stops.
            REQUIRE_FALSE(IsFrontier(FineRoads::Snapshot(), 100.0f, 0.0f));
        }
    }

    SECTION("when the edge link is a ledge rather than a portal")
    {
        // The record format puts both in the same slot, told apart by a type
        // field. Following a ledge as though it were a portal would link a
        // road to whatever triangle index the ledge data happens to hold.
        here.ledge[0] = true;
        engine.AddNavMesh(cell, kMeshA, {here});
        engine.AddNavMesh(cell, kMeshB, {there});
        Poll();

        SECTION("should leave the node unlinked")
        {
            REQUIRE(FineRoads::Snapshot().adjacency[0].empty());
        }
    }

    SECTION("when the edge link points past the end of the portal table")
    {
        here.danglingLink[0] = true;
        engine.AddNavMesh(cell, kMeshA, {here});
        engine.AddNavMesh(cell, kMeshB, {there});
        Poll();

        SECTION("should ignore the edge rather than read past the table")
        {
            REQUIRE(FineRoads::Snapshot().adjacency[0].empty());
        }
    }
}

TEST_CASE("FineRoads marks where coverage ends", "[FineRoads][engine]")
{
    // Happy path, re-run per leaf: one cell with one road triangle whose road
    // leaves through a portal.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    engine.LoadGrid({cell});

    auto edgeOfTown = RoadAt(100.0f, 0.0f);

    SECTION("when the road leaves for a mesh nobody has read")
    {
        edgeOfTown.portalMesh[0] = kUnloadedMesh;
        edgeOfTown.portalTriangle[0] = 0;
        engine.AddNavMesh(cell, kMeshA, {edgeOfTown});
        Poll();

        SECTION("should mark the node a frontier")
        {
            // This is the handoff to the coarse long-distance graph, and the
            // only place a route may leave the fine one. A road that reaches
            // the edge of coverage without being marked is a route that stops
            // dead in open country.
            REQUIRE(IsFrontier(FineRoads::Snapshot(), 100.0f, 0.0f));
        }
    }

    SECTION("when the road simply stops")
    {
        engine.AddNavMesh(cell, kMeshA, {edgeOfTown});
        Poll();

        SECTION("should not mark the node")
        {
            REQUIRE_FALSE(IsFrontier(FineRoads::Snapshot(), 100.0f, 0.0f));
        }
    }
}

TEST_CASE("FineRoads follows the loaded grid", "[FineRoads][engine]")
{
    // Happy path, re-run per leaf: two loaded cells, each carrying one piece
    // of road, and a graph already built from both.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* here = engine.AddExteriorCell(space, 0, 0, nullptr);
    auto* next = engine.AddExteriorCell(space, 1, 0, nullptr);
    engine.AddNavMesh(here, kMeshA, {RoadAt(100.0f, 0.0f)});
    engine.AddNavMesh(next, kMeshB, {RoadAt(4200.0f, 0.0f)});
    engine.LoadGrid({here, next});
    Poll();

    SECTION("when both cells are loaded")
    {
        SECTION("should build from all of them")
        {
            // The grid is nine or twenty-five cells and shifts as a unit. A
            // graph built from one of them covers a ninth of the ground the
            // player can see.
            REQUIRE(FineRoads::NodeCount() == 2);
        }

        SECTION("should name the worldspace it covers")
        {
            // A route is only meaningful within one worldspace, so the
            // consumer has to be able to tell which one this is.
            REQUIRE(FineRoads::Snapshot().worldSpace == kTamriel);
        }
    }

    SECTION("when one of them unloads")
    {
        engine.LoadGrid({here});
        Poll();

        SECTION("should drop its road")
        {
            // Nodes for ground that is no longer loaded route a traveller
            // through cells the engine has taken down.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }

    SECTION("when a cell in the grid has not finished loading")
    {
        engine.SetCellAttached(next, false);
        engine.LoadGrid({here, next});
        Poll();

        SECTION("should leave it out")
        {
            // A cell appears in the grid before its contents arrive, and its
            // navmesh is not there to read yet.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }

    SECTION("when a cell in the grid is an interior")
    {
        engine.SetCellInterior(next, true);
        engine.LoadGrid({here, next});
        Poll();

        SECTION("should leave it out")
        {
            // Interiors have navmesh, and it is not road: a house floor would
            // arrive as a knot of nodes inside a building.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }

    SECTION("when the player goes indoors")
    {
        engine.grid.playerIndoors = true;
        Poll();

        SECTION("should clear the graph")
        {
            // There is no exterior grid to read. Keeping the last outdoor
            // graph would answer questions about wherever the player was
            // standing before they went inside.
            REQUIRE(FineRoads::NodeCount() == 0);
        }
    }

    SECTION("when the grid is not up yet")
    {
        engine.grid.present = false;
        Poll();

        SECTION("should keep what it had")
        {
            // Distinct from going indoors: this is a sample that failed rather
            // than a sample that says the player is inside, and throwing away
            // a good graph over it would blank the road network during every
            // load screen.
            REQUIRE(FineRoads::NodeCount() == 2);
        }
    }

    SECTION("when the world is not up yet")
    {
        engine.terrain.tesPresent = false;
        Poll();

        SECTION("should keep what it had")
        {
            REQUIRE(FineRoads::NodeCount() == 2);
        }
    }
}

TEST_CASE("FineRoads remembers the cells it has read", "[FineRoads][engine]")
{
    // Happy path, re-run per leaf: one loaded cell with one piece of road,
    // already extracted — and then a second navmesh appears on it, which only
    // a fresh read of the cell could ever find.
    //
    // Both leaves have to move the grid as well, because the active graph is
    // rebuilt on a change of active cells and not on a change of their
    // contents. Without that the cell could be re-read every poll and the
    // graph would look the same either way.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    engine.AddNavMesh(cell, kMeshA, {RoadAt(100.0f, 0.0f)});
    engine.LoadGrid({cell});
    Poll();
    REQUIRE(FineRoads::NodeCount() == 1);
    engine.AddNavMesh(cell, kMeshB, {RoadAt(200.0f, 0.0f)});

    SECTION("when the grid grows around it")
    {
        auto* neighbour = engine.AddExteriorCell(space, 1, 0, nullptr);
        engine.LoadGrid({cell, neighbour});
        Poll();

        SECTION("should not read it a second time")
        {
            // Extraction walks every triangle of every mesh in the cell, and
            // the grid moves every time the player crosses a boundary. Paying
            // that again for the cells that did not change is most of the cost
            // of the module, over and over, for nothing.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }

    SECTION("when the cell unloads and the player comes back")
    {
        // The grid shifts as a unit, so an unload arrives as a grid holding
        // different cells rather than as an empty one.
        auto* elsewhere = engine.AddExteriorCell(space, 8, 0, nullptr);
        engine.LoadGrid({elsewhere});
        Poll();
        REQUIRE(FineRoads::NodeCount() == 0);
        engine.LoadGrid({cell});
        Poll();

        SECTION("should restore it from the cache")
        {
            // Backtracking across a cell boundary is constant, and a cache
            // that dropped what left the grid would pay the extraction again
            // every time the player turned around.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }
}

TEST_CASE("FineRoads rescans on a schedule", "[FineRoads][engine]")
{
    // Happy path, re-run per leaf: one loaded cell with road, a graph built
    // from it, and a second cell that has just come into the grid without
    // anything having announced it.
    //
    // Cell loads do have an event; cell UNLOADS have none, which is why there
    // is a backstop at all — an unload unaccompanied by a load would otherwise
    // leave the cell in the active set for good.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    FineRoads::Initialize();
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* here = engine.AddExteriorCell(space, 0, 0, nullptr);
    auto* next = engine.AddExteriorCell(space, 1, 0, nullptr);
    engine.AddNavMesh(here, kMeshA, {RoadAt(100.0f, 0.0f)});
    engine.AddNavMesh(next, kMeshB, {RoadAt(4200.0f, 0.0f)});
    engine.LoadGrid({here});
    Poll();
    REQUIRE(FineRoads::NodeCount() == 1);
    engine.LoadGrid({here, next});

    SECTION("when no time has passed and nothing has been announced")
    {
        Poll(0.1);

        SECTION("should not resample the grid")
        {
            // The poll runs twice a second forever. Sampling the grid every
            // time is a main-thread hop per poll for a world that has not
            // changed.
            REQUIRE(FineRoads::NodeCount() == 1);
        }
    }

    SECTION("when the backstop interval goes by")
    {
        Poll(0.1);
        Poll(2.0);

        SECTION("should resample the grid")
        {
            REQUIRE(FineRoads::NodeCount() == 2);
        }
    }

    SECTION("when the engine says a cell has finished loading")
    {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        REQUIRE(holder != nullptr);
        RE::TESCellFullyLoadedEvent loaded{};
        holder->GetEventSource<RE::TESCellFullyLoadedEvent>()->SendEvent(&loaded);
        Poll(0.1);

        SECTION("should resample on the very next poll")
        {
            // Waiting out the backstop instead would leave the player driving
            // a road graph that stops at the edge of where they were a moment
            // ago, for as long as the interval lasts.
            REQUIRE(FineRoads::NodeCount() == 2);
        }
    }
}

TEST_CASE("FineRoads does nothing while it is switched off", "[FineRoads][engine]")
{
    // The one road here sits somewhere no other case puts one, because the
    // module has no reset: whatever graph the process last built is still
    // there, and the question is only whether this cell reached it.
    constexpr float kNowhereElse = 90000.0f;

    EngineMock engine;
    const ConfiguredSettings settings{"[FineRoads]\nbFineRoadsEnabled=0\nbFineRoadsDebugBitmap=0\n"};
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    engine.AddNavMesh(cell, kMeshA, {RoadAt(kNowhereElse, kNowhereElse)});
    engine.LoadGrid({cell});

    SECTION("when the poll comes round")
    {
        Poll();

        SECTION("should read nothing")
        {
            // This is still experimental, and the killswitch is how it stays
            // out of a player's way: it has to cost nothing, not merely be
            // ignored downstream.
            REQUIRE(NodeAt(FineRoads::Snapshot(), kNowhereElse, kNowhereElse) == FineRoads::kInvalidNode);
        }
    }
}

TEST_CASE("FineRoads draws its graph on request", "[FineRoads][engine]")
{
    // The bitmap is how the graph is checked against the game world, which is
    // the whole reason the module reports before anything routes over it.
    const std::filesystem::path logDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};
    const auto bitmap = logDir / "NarrativeEngine_FineRoads.bmp";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    std::filesystem::remove(bitmap, ec);

    // Five pieces of road rather than one or two: a write is skipped when the
    // graph is the same shape as the last one dumped, and no other case in this
    // file builds five nodes.
    EngineMock engine;
    auto* space = engine.AddWorldSpace(kTamriel);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    engine.AddNavMesh(cell,
                      kMeshA,
                      {RoadAt(0.0f, 0.0f),
                       RoadAt(1000.0f, 0.0f),
                       RoadAt(2000.0f, 0.0f),
                       RoadAt(3000.0f, 0.0f),
                       RoadAt(4000.0f, 0.0f)});
    engine.LoadGrid({cell});

    SECTION("when the dump is turned on")
    {
        const ConfiguredSettings settings{"[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                          "bFineRoadsDebugBitmap=1\niFineRoadsBitmapUnitsPerPixel=64\n"};
        Poll();

        SECTION("should write the image")
        {
            REQUIRE(std::filesystem::exists(bitmap));
        }
    }

    SECTION("when the dump is turned off")
    {
        const ConfiguredSettings settings{kSettings};
        Poll();

        SECTION("should write nothing")
        {
            // It is a diagnostic, and writing a bitmap out of a poll that runs
            // twice a second is not something a player should pay for.
            REQUIRE_FALSE(std::filesystem::exists(bitmap));
        }
    }
}
