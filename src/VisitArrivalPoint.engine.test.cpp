#include <VisitArrivalPoint.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FineRoads.h>
#include <PluginThread.h>
#include <StuckRecovery.h>
#include <ThreadRole.h>
#include <TravelGraph.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

// Tests for where a visitor appears.
//
// The beat warps someone near the player and lets them walk the rest of the
// way, and the whole claim of the design is that the warp lands somewhere they
// could have walked FROM — up the road, from the direction they live. Getting
// that wrong is not a crash; it is a visitor from Riften strolling out of the
// western snowfields, which reads as a spawn and cannot be recovered from
// afterwards. So the headline case here is not that a point is found but that
// moving the sender's home to the other end of the road moves the arrival with
// it.
//
// Under that sit the gates, which are few because the candidates are already
// road: a node is a preferred navmesh triangle, so standable ground is a
// property of where it came from rather than something to re-derive. What is
// left to reject is distance, ground that has stopped being loaded, and a spot
// the player can see.
//
// Then the ladder. There is usually no fine road data (indoors, a save just
// loaded, open country away from a road), so the search drops to a bearing off
// the coarse skeleton, and where even that is missing it declines. Declining
// matters as much as succeeding: the one fallback deliberately NOT built is
// "arrive from anywhere", because that is the failure the module exists to
// prevent and shipping it as a safety net would make the worst case the thing
// we set out to fix.
//
// ONE COARSE GRAPH PER PROCESS. TravelGraph::Initialize is one-shot with no
// reset, so a test case builds its skeleton once above its sections. ctest
// gives every test case its own process; running the executable directly puts
// them in one and the first graph built wins.

namespace
{
    namespace VisitArrivalPoint = NarrativeEngine::VisitArrivalPoint;
    namespace FineRoads = NarrativeEngine::FineRoads;
    namespace TravelGraph = NarrativeEngine::TravelGraph;
    namespace StuckRecovery = NarrativeEngine::StuckRecovery;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using Triangle = EngineMock::FakeTriangle;

    // The band is the shipped default. Cover radius is one actor's width, and
    // the bearing fallback is on unless a case says otherwise.
    constexpr const char* kSettings = "[Beats]\niVisitMarkerMinDistanceUnits=800\n"
                                      "iVisitMarkerMaxDistanceUnits=2500\n"
                                      "iVisitArrivalCoverRadiusUnits=64\n"
                                      "bVisitArrivalAllowCoarseBearing=true\n"
                                      "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                      "bFineRoadsDebugBitmap=0\n"
                                      "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    constexpr std::uint32_t kWorld = 0x00D00001u;
    constexpr std::uint32_t kOtherWorld = 0x00D00002u;
    constexpr std::uint32_t kRoadMesh = 0x00D01001u;
    constexpr std::uint32_t kGroundMesh = 0x00D01002u;
    constexpr std::uint32_t kNavi = 0x00D01010u;
    constexpr std::uint32_t kPlayer = 0x00000014u;
    constexpr std::uint32_t kSender = 0x00D02001u;

    // Ground is flat at zero, and the search stands the sender just clear of
    // it — the same lift StuckRecovery applies to anything it places.
    constexpr float kGround = 0.0f;
    constexpr float kStandingZ = kGround + StuckRecovery::kGroundClearanceUnits;

    // A straight road through the player, running east-west, with a node every
    // 500 units out to 3500 either side. Deliberately symmetric: which half of
    // it the search walks is decided only by where the visitor lives.
    constexpr float kNodeSpacing = 500.0f;
    constexpr int kNodesPerSide = 7;
    constexpr float kRoadEnd = kNodeSpacing * kNodesPerSide; // 3500

    // Wide enough to hold the whole road and every bearing sample.
    constexpr float kWorldEdge = 30000.0f;

    RE::NiPoint3 At(float x, float y)
    {
        return RE::NiPoint3{x, y, kGround};
    }

    // Lay the road as one navmesh of preferred triangles, chained end to end.
    // Index 0 is the westernmost node; the player stands on the middle one.
    void LayRoad(EngineMock& engine, RE::TESObjectCELL* cell)
    {
        std::vector<Triangle> road;
        const int count = kNodesPerSide * 2 + 1;
        for (int i = 0; i < count; ++i) {
            Triangle t;
            t.x = -kRoadEnd + static_cast<float>(i) * kNodeSpacing;
            t.y = 0.0f;
            t.z = kGround;
            t.neighbor[0] = i > 0 ? i - 1 : -1;
            t.neighbor[1] = i < count - 1 ? i + 1 : -1;
            road.push_back(t);
        }
        engine.AddNavMesh(cell, kRoadMesh, road);
    }

    // The ground the gates ask about. Separate from the road cell on purpose:
    // TES::GetCell answers every world position with the fabricated ground
    // cell, while FineRoads reads the cells in the loaded grid, so the patch
    // that makes positions standable never becomes road data itself.
    void LayGround(EngineMock& engine)
    {
        engine.world.cellIsInterior = false;
        engine.terrain.landHeight = kGround;
        engine.AddNavmeshPatch(
            engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, kWorldEdge, kGround);
    }

    // Every ray from the camera blocked, so cover exists everywhere and the
    // only thing left to decide is which node wins.
    void CoverEverywhere(EngineMock& engine)
    {
        engine.visibility.pickHitFraction = 0.0f;
    }

    void PollFineRoads()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([](const PluginThread::Token& pt) { FineRoads::Poll(pt, 1000.0); });
    }

    RE::Actor* ActorAt(EngineMock& engine, std::uint32_t formID, RE::TESObjectCELL* cell, RE::NiPoint3 position)
    {
        auto* actor = engine.AddActor(formID);
        actor->parentCell = cell;
        actor->data.location = position;
        return actor;
    }

    // Find reads engine state through main-thread hops and so has to be called
    // holding a plugin token. The only way to hold one is to be handed it.
    VisitArrivalPoint::Result FindFor(RE::Actor* sender, RE::Actor* player)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        VisitArrivalPoint::Result out;
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { out = VisitArrivalPoint::Find(pt, sender, player); });
        return out;
    }

    float Dist2D(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }
} // namespace

TEST_CASE("VisitArrivalPoint brings the visitor in from the side of the road they live on",
          "[VisitArrivalPoint][engine]")
{
    // A road through the player running both ways, usable ground under all of
    // it, and cover everywhere — so nothing but the visitor's home can decide
    // which direction the arrival comes from.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);
    REQUIRE(FineRoads::NodeCount() == static_cast<std::size_t>(kNodesPerSide * 2 + 1));

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));

    SECTION("when the visitor lives east")
    {
        auto* sender = ActorAt(engine, kSender, cell, At(kRoadEnd, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should arrive from the east")
        {
            REQUIRE(result.Ok());
            // The claim the whole phase rests on. A visitor who appears on the
            // wrong side of the player has not travelled to them, and no
            // amount of walking afterwards repairs the impression.
            REQUIRE(result.point.x > 0.0f);
        }

        SECTION("should arrive on the road rather than beside it")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::FineRoad);
            REQUIRE(std::fabs(result.point.y) < 1.0f);
        }

        SECTION("should take the nearest usable node rather than the farthest")
        {
            REQUIRE(result.Ok());
            // Nodes sit every 500 units; the first at or past the 800-unit
            // floor is the one at 1000.
            REQUIRE(std::fabs(result.point.x - 1000.0f) < 1.0f);
        }

        SECTION("should check for cover over a whole visitor, not just their feet")
        {
            REQUIRE(result.Ok());
            // The gate samples 10/50/90 percent of the height it is given,
            // so the top ray lands at 0.9 of it. A tester watched an NPC
            // appear on a spot that passed while their head was plainly
            // visible: the rays had swept to 115 units and the visitor was
            // taller than that.
            //
            // Asserted as a reach over a crown rather than as the constant
            // itself, because what matters is that the fan clears a tall
            // visitor's head and not what number produces that.
            constexpr float kTallestVisitorCrownUnits = 140.0f;
            REQUIRE(engine.visibility.highestPickTargetZ >= result.point.z + kTallestVisitorCrownUnits);
        }

        SECTION("should stand the visitor clear of the ground")
        {
            REQUIRE(result.Ok());
            REQUIRE(std::fabs(result.point.z - kStandingZ) < 0.1f);
        }

        SECTION("should keep the road behind it as fallbacks, further out each time")
        {
            REQUIRE(result.Ok());
            REQUIRE_FALSE(result.fallbacks.empty());
            // Road order, not distance order — the escort walks a stuck
            // visitor BACK ALONG THEIR OWN ROUTE, and re-sorting these would
            // scatter them onto unrelated ground instead.
            float previous = result.point.x;
            for (const auto& fallback : result.fallbacks) {
                REQUIRE(fallback.x > previous);
                previous = fallback.x;
            }
        }
    }

    SECTION("when the visitor lives west")
    {
        auto* sender = ActorAt(engine, kSender, cell, At(-kRoadEnd, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should arrive from the west instead")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x < 0.0f);
        }

        SECTION("should mirror the eastern answer exactly")
        {
            REQUIRE(result.Ok());
            // Same road, same gates, opposite end. Anything asymmetric here is
            // a bias in the walk rather than a fact about the world.
            REQUIRE(std::fabs(result.point.x + 1000.0f) < 1.0f);
        }
    }
}

TEST_CASE("VisitArrivalPoint keeps to the road the visitor would actually walk", "[VisitArrivalPoint][engine]")
{
    // Where the visitor lives and which way they arrive from are not the
    // same question, and this is the case that separates them.
    //
    // The sender lives far to the EAST. The road to them leaves to the
    // WEST and loops round -- ordinary geography, and the reason a
    // "candidate must be closer to home than the player is" rule was
    // rejected: it would refuse the entire first half of a journey like
    // this one.
    //
    // So the direction has to come from the route rather than from the
    // bearing. The coarse path decides it, and the fine road between the
    // player and that route's way in is the only stretch offered. Route
    // at the sender instead, as this used to, and the answer swings to
    // the eastern road that goes nowhere near them.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    // A coarse chain that runs west from the player and only then turns
    // back east to where the sender lives.
    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00D06000u;
    std::vector<const RE::BSNavmeshInfo*> chain;
    for (int cellX : {0, -1, -2, -3, 10}) {
        chain.push_back(
            engine.AddNavmeshInfo(navi, nextForm++, kWorld, static_cast<std::int16_t>(cellX), 0, 0.0f, 0.0f, 0.0f));
    }
    engine.AddPreferredPath(navi, chain);
    TravelGraph::Initialize();
    REQUIRE(TravelGraph::NodeCount() > 0);

    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);

    // The fine road has to lie under the coarse one, or the player is
    // off-road and every candidate is a hop sideways onto it. Coarse
    // nodes come out at cell centres, so this runs along y=2048 through
    // the first of them, reaching both ways.
    {
        std::vector<Triangle> fineRoad;
        const int count = 15;
        for (int i = 0; i < count; ++i) {
            Triangle t;
            t.x = 2048.0f + (static_cast<float>(i) - 7.0f) * kNodeSpacing;
            t.y = 2048.0f;
            t.z = kGround;
            t.neighbor[0] = i > 0 ? i - 1 : -1;
            t.neighbor[1] = i < count - 1 ? i + 1 : -1;
            // Both ends leave the loaded region, which is what gives
            // RoadRoute a handoff to choose between -- and choosing it
            // by journey cost is exactly what used to send the visitor
            // up the wrong one.
            if (i == 0 || i == count - 1) {
                t.portalMesh[2] = 0x00D0FFFFu;
                t.portalTriangle[2] = 0;
            }
            fineRoad.push_back(t);
        }
        engine.AddNavMesh(cell, kRoadMesh, fineRoad);
    }
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    const RE::NiPoint3 playerPos = At(2048.0f, 2048.0f);
    auto* player = ActorAt(engine, kPlayer, cell, playerPos);

    SECTION("when the road to them leaves in the opposite direction")
    {
        // Far east as the crow flies; reached by walking west.
        auto* sender = ActorAt(engine, kSender, cell, At(43008.0f, 2048.0f));
        const auto result = FindFor(sender, player);

        SECTION("should follow the route west, not the bearing east")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x < playerPos.x);
        }
    }
}

TEST_CASE("VisitArrivalPoint keeps the arrival inside the distance band", "[VisitArrivalPoint][engine]")
{
    // A band narrow enough that exactly one node on each side of the player
    // sits inside it, so both edges are answered by the same fixture.
    constexpr const char* kNarrowBand = "[Beats]\niVisitMarkerMinDistanceUnits=1200\n"
                                        "iVisitMarkerMaxDistanceUnits=1700\n"
                                        "iVisitArrivalCoverRadiusUnits=64\n"
                                        "bVisitArrivalAllowCoarseBearing=true\n"
                                        "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                        "bFineRoadsDebugBitmap=0\n"
                                        "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    EngineMock engine;
    const ConfiguredSettings settings{kNarrowBand};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
    auto* sender = ActorAt(engine, kSender, cell, At(kRoadEnd, 0.0f));
    const auto result = FindFor(sender, player);

    SECTION("should skip the nodes nearer than the minimum")
    {
        REQUIRE(result.Ok());
        // 500 and 1000 are both on the road and both usable; they are simply
        // too close for the walk in to read as an approach.
        REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) >= 1200.0f);
    }

    SECTION("should skip the nodes past the maximum")
    {
        REQUIRE(result.Ok());
        REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) <= 1700.0f);
        for (const auto& fallback : result.fallbacks) {
            REQUIRE(Dist2D(fallback, At(0.0f, 0.0f)) <= 1700.0f);
        }
    }

    SECTION("should land on the one node the band admits")
    {
        REQUIRE(result.Ok());
        REQUIRE(std::fabs(result.point.x - 1500.0f) < 1.0f);
        REQUIRE(result.fallbacks.empty());
    }
}

TEST_CASE("VisitArrivalPoint reaches past the band rather than declining", "[VisitArrivalPoint][engine]")
{
    // The band's ceiling says how long the walk in ought to take. It is
    // a preference, not a rule about where a person may stand, and
    // treating it as a rule meant a road whose only hidden stretch sat
    // slightly too far out produced no visit at all.
    //
    // The road here runs east with cover only beyond the ceiling, which
    // is the shape that was losing visits.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
    auto* sender = ActorAt(engine, kSender, cell, At(kRoadEnd, 0.0f));

    SECTION("when the band admits nothing but the road continues")
    {
        // A band that ends before the first road node does.
        const ConfiguredSettings narrow{"[Beats]\niVisitMarkerMinDistanceUnits=100\n"
                                        "iVisitMarkerMaxDistanceUnits=300\n"
                                        "iVisitArrivalCoverRadiusUnits=64\n"
                                        "bVisitArrivalAllowCoarseBearing=false\n"
                                        "[FineRoads]\nbFineRoadsEnabled=1\n"
                                        "iFineRoadsBackstopSeconds=1\nbFineRoadsDebugBitmap=0\n"
                                        "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n"};
        const auto result = FindFor(sender, player);

        SECTION("should take a point past the ceiling instead of giving up")
        {
            REQUIRE(result.Ok());
            REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) > 300.0f);
        }
    }

    SECTION("when the band admits a point")
    {
        const auto result = FindFor(sender, player);

        SECTION("should still prefer it over anything further out")
        {
            // The reach is a fallback and must not become the default:
            // a visitor who could have arrived at 1000 units should not
            // be put at 5000 because the search liked it better.
            REQUIRE(result.Ok());
            REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) <= 2500.0f);
        }
    }
}

TEST_CASE("VisitArrivalPoint will use open ground the player is not facing", "[VisitArrivalPoint][engine]")
{
    // On a plain there is nothing to stand behind, and demanding cover
    // there means declining most visits. But cover is only ever a proxy
    // for "the player will not watch this happen" -- and someone facing
    // the other way is not watching either.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    // Wide open: every ray reaches, so nowhere is covered.
    engine.visibility.pickHitFraction = 1.0f;

    auto* sender = ActorAt(engine, kSender, cell, At(kRoadEnd, 0.0f));

    SECTION("when the player is facing away from the road east")
    {
        // Facing west: angle.z is clockwise from +Y, so 270 degrees.
        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        player->data.angle.z = 3.0f * 3.14159265f / 2.0f;
        const auto result = FindFor(sender, player);

        SECTION("should place them on the open road behind them")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x > 0.0f);
        }

        SECTION("should keep them far enough back to be unnoticeable")
        {
            REQUIRE(result.Ok());
            REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) >= 3000.0f);
        }
    }

    SECTION("when the player is looking straight down that road")
    {
        // Facing east, which is where the only candidates are.
        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        player->data.angle.z = 3.14159265f / 2.0f;
        const auto result = FindFor(sender, player);

        SECTION("should decline rather than let them appear in plain sight")
        {
            // The concession is to the player's attention, not to the
            // difficulty of finding cover. Facing the spot puts it back.
            REQUIRE_FALSE(result.Ok());
        }
    }
}

TEST_CASE("VisitArrivalPoint refuses ground the player can see", "[VisitArrivalPoint][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    // Every ray reaches its endpoint: open ground, nothing to hide behind.
    engine.visibility.pickHitFraction = 1.0f;

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
    // Looking east, straight down the only stretch of road the sender
    // could arrive on. Facing matters now: open ground the player is
    // NOT watching is usable, so this case has to actually be watched.
    player->data.angle.z = 3.14159265f / 2.0f;
    auto* sender = ActorAt(engine, kSender, cell, At(kRoadEnd, 0.0f));

    SECTION("should decline rather than let the player watch the arrival")
    {
        const auto result = FindFor(sender, player);
        // A visible arrival is worse than no visit: the player sees the seams
        // of the mod, and every visit after it is read the same way.
        REQUIRE_FALSE(result.Ok());
        REQUIRE(result.tier == VisitArrivalPoint::Tier::None);
    }
}

TEST_CASE("VisitArrivalPoint refuses road that has stopped being navmesh", "[VisitArrivalPoint][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    // Ground and cover, but no navmesh patch under any of it. The fine graph
    // still remembers a road here, because it is a snapshot and the cells it
    // was built from unload out from under it.
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
    auto* sender = ActorAt(engine, kSender, cell, At(kRoadEnd, 0.0f));

    SECTION("should decline rather than place the visitor off the mesh")
    {
        const auto result = FindFor(sender, player);
        REQUIRE_FALSE(result.Ok());
    }
}

TEST_CASE("VisitArrivalPoint falls back to the bearing home when there is no road", "[VisitArrivalPoint][engine]")
{
    // No fine graph at all — the ordinary case indoors, on a fresh load, and
    // anywhere off the road network. What is left is the coarse skeleton, and
    // all it can honestly supply is a direction.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00D03000u;
    std::vector<const RE::BSNavmeshInfo*> road;
    for (int cellX = 0; cellX <= 5; ++cellX) {
        road.push_back(
            engine.AddNavmeshInfo(navi, nextForm++, kWorld, static_cast<std::int16_t>(cellX), 0, 0.0f, 0.0f, 0.0f));
    }
    engine.AddPreferredPath(navi, road);
    TravelGraph::Initialize();
    REQUIRE(TravelGraph::NodeCount() > 0);

    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayGround(engine);
    CoverEverywhere(engine);
    REQUIRE(FineRoads::NodeCount() == 0);

    // Standing on the first coarse node, so the bearing is decided by the
    // second one and runs due east along the skeleton.
    const RE::NiPoint3 playerPos = At(2048.0f, 2048.0f);
    auto* player = ActorAt(engine, kPlayer, cell, playerPos);
    auto* sender = ActorAt(engine, kSender, cell, At(20480.0f, 2048.0f));
    const auto result = FindFor(sender, player);

    SECTION("should still place the visitor toward home")
    {
        REQUIRE(result.Ok());
        REQUIRE(result.tier == VisitArrivalPoint::Tier::CoarseBearing);
        REQUIRE(result.point.x > playerPos.x);
    }

    SECTION("should aim straight down the bearing when the ground allows it")
    {
        REQUIRE(result.Ok());
        // The arc is sampled either side of the bearing and scored on how far
        // off it a candidate is, so on open usable ground the winner is the
        // one dead on it.
        REQUIRE(std::fabs(result.point.y - playerPos.y) < 1.0f);
    }

    SECTION("should stay inside the band")
    {
        REQUIRE(result.Ok());
        const float distance = Dist2D(result.point, playerPos);
        REQUIRE(distance >= 800.0f);
        REQUIRE(distance <= 2500.0f);
    }
}

TEST_CASE("VisitArrivalPoint declines when the bearing fallback is switched off", "[VisitArrivalPoint][engine]")
{
    constexpr const char* kRoadOnly = "[Beats]\niVisitMarkerMinDistanceUnits=800\n"
                                      "iVisitMarkerMaxDistanceUnits=2500\n"
                                      "iVisitArrivalCoverRadiusUnits=64\n"
                                      "bVisitArrivalAllowCoarseBearing=false\n"
                                      "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                      "bFineRoadsDebugBitmap=0\n"
                                      "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    EngineMock engine;
    const ConfiguredSettings settings{kRoadOnly};

    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00D03000u;
    std::vector<const RE::BSNavmeshInfo*> road;
    for (int cellX = 0; cellX <= 5; ++cellX) {
        road.push_back(
            engine.AddNavmeshInfo(navi, nextForm++, kWorld, static_cast<std::int16_t>(cellX), 0, 0.0f, 0.0f, 0.0f));
    }
    engine.AddPreferredPath(navi, road);
    TravelGraph::Initialize();

    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(2048.0f, 2048.0f));
    auto* sender = ActorAt(engine, kSender, cell, At(20480.0f, 2048.0f));

    SECTION("should hold out for a road rather than arrive off one")
    {
        const auto result = FindFor(sender, player);
        // The stricter reading of "visitors arrive by road". Ground that would
        // have been perfectly usable is refused because nothing can say it is
        // a route anyone walks.
        REQUIRE_FALSE(result.Ok());
    }
}

TEST_CASE("VisitArrivalPoint declines when nothing can route the two ends together", "[VisitArrivalPoint][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* elsewhere = engine.AddWorldSpace(kOtherWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    auto* farCell = engine.AddExteriorCell(elsewhere, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));

    SECTION("when the visitor is in another worldspace entirely")
    {
        auto* sender = ActorAt(engine, kSender, farCell, At(0.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should decline rather than route between them")
        {
            // Solstheim to Skyrim is a boat, not a walk. Every graph here is
            // per-worldspace, and a plan across two of them would be fiction.
            REQUIRE_FALSE(result.Ok());
        }
    }

    SECTION("when the visitor has no cell at all")
    {
        auto* sender = ActorAt(engine, kSender, nullptr, At(0.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should decline rather than guess an origin")
        {
            REQUIRE_FALSE(result.Ok());
        }
    }

    SECTION("when there is no visitor")
    {
        SECTION("should decline rather than dereference one")
        {
            REQUIRE_FALSE(FindFor(nullptr, player).Ok());
        }
    }
}

TEST_CASE("VisitArrivalPoint finds a visitor who is not loaded", "[VisitArrivalPoint][engine]")
{
    // The case the beat is FOR, and the one the first in-game run died on.
    //
    // A visit brings somebody from elsewhere, so the sender is almost never
    // attached: GetParentCell is `return parentCell` and reads null for any
    // reference the engine has not loaded, and an unloaded interior cannot be
    // walked for its load door either. Resolving a visitor's end of the route
    // the way the player's end is resolved therefore works for exactly the
    // visitor who did not need bringing.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));

    SECTION("when they are away and the save still knows which cell holds them")
    {
        // No parent cell -- nothing has attached them -- but the save has
        // them filed east, which is where they would be walking from.
        auto* sender = ActorAt(engine, kSender, nullptr, At(kRoadEnd, 0.0f));
        engine.SetSaveParentCell(sender, cell);
        const auto result = FindFor(sender, player);

        SECTION("should still bring them in from the east")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x > 0.0f);
        }
    }

    SECTION("when the save has them indoors, in a room with no marker of its own")
    {
        // The case every real dispatch hit. College students are filed in
        // the Hall of Attainment: an interior, so there is no exterior
        // position to read, and its own Location carries no map marker --
        // the College above it does. Stopping at the room resolves for
        // nobody who lives indoors, which is most people.
        auto* sender = ActorAt(engine, kSender, nullptr, At(0.0f, 0.0f));
        auto* hall = engine.AddLocation(0x00D05010u, "Hall of Attainment", {}, "HallOfAttainmentLocation");
        auto* college = engine.AddLocation(0x00D05011u, "College of Winterhold", {}, "CollegeLocation");
        engine.SetLocationParent(hall, college);
        // Only the parent is pinned to the map, and it is pinned west.
        engine.SetLocationMarker(college, cell, At(-kRoadEnd, 0.0f));

        auto* room = engine.AddExteriorCell(space, 9, 9, hall);
        engine.SetCellInterior(room, true);
        engine.SetSaveParentCell(sender, room);
        const auto result = FindFor(sender, player);

        SECTION("should climb to the marker above it and come in from the west")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x < 0.0f);
        }
    }

    SECTION("when their home's map marker is itself in an unloaded cell")
    {
        // The whole point of the marker rung is to place somebody who is
        // far away -- and a marker that far away is in an unloaded cell
        // too, so reading its parent cell gives nothing. Resolving the
        // marker but not its cell is the shape that made every College
        // dispatch decline after the parentLoc climb was already working.
        auto* sender = ActorAt(engine, kSender, nullptr, At(0.0f, 0.0f));
        auto* home = engine.AddLocation(0x00D05020u, "Somewhere Far", {}, "FarLocation");

        // A marker with no attached cell, the way an unloaded reference
        // reads, but which the save can still place.
        auto* marker = engine.AddReference(nullptr, 0x00D05021u, At(-kRoadEnd, 0.0f));
        engine.SetSaveParentCell(marker, cell);
        engine.SetLocationMarkerRef(home, marker);
        engine.SetEditorLocation(sender, home);
        const auto result = FindFor(sender, player);

        SECTION("should still place them, from the side the marker is on")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x < 0.0f);
        }
    }

    SECTION("when not even the save can place them")
    {
        // Asleep in some interior on the far side of the province. All that
        // is left is the location the record files them under, and its map
        // marker -- coarse, but it is a real place and it is theirs.
        auto* sender = ActorAt(engine, kSender, nullptr, At(0.0f, 0.0f));
        auto* home = engine.AddLocation(0x00D05002u, "Sender's Home", {}, "SenderHome");
        engine.SetLocationMarker(home, cell, At(-kRoadEnd, 0.0f));
        engine.SetEditorLocation(sender, home);
        const auto result = FindFor(sender, player);

        SECTION("should bring them in from the side their home is on")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x < 0.0f);
        }
    }

    SECTION("when nothing anywhere can place them")
    {
        // No cell, no editor location, and no ambient current location
        // either -- the mock hands every reference the same one, and
        // leaving it in play means this case is really asking whether
        // THAT has a map marker.
        engine.world.playerHasLocation = false;
        auto* sender = ActorAt(engine, kSender, nullptr, At(0.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should decline rather than invent a direction")
        {
            REQUIRE_FALSE(result.Ok());
        }
    }
}

TEST_CASE("VisitArrivalPoint measures a visitor indoors from their doorstep", "[VisitArrivalPoint][engine]")
{
    // Someone at home is nowhere on the road network — their interior has no
    // place on it. What decides the direction is where their door comes out,
    // which is also where they would actually emerge.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
    auto* house = engine.AddExteriorCell(space, 9, 9, nullptr);
    engine.SetCellInterior(house, true);

    SECTION("when their door comes out east of the player")
    {
        engine.AddLoadDoor(house, 0x00D04001u, cell, At(kRoadEnd, 0.0f));
        // Interior coordinates are small and local and say nothing about
        // where the building sits in the world.
        auto* sender = ActorAt(engine, kSender, house, At(100.0f, 100.0f));
        const auto result = FindFor(sender, player);

        SECTION("should bring them in from the east")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x > 0.0f);
        }
    }

    SECTION("when their door comes out west of the player")
    {
        engine.AddLoadDoor(house, 0x00D04002u, cell, At(-kRoadEnd, 0.0f));
        auto* sender = ActorAt(engine, kSender, house, At(100.0f, 100.0f));
        const auto result = FindFor(sender, player);

        SECTION("should bring them in from the west instead")
        {
            // Same interior, same interior position, opposite answer — which
            // is the whole reason the origin is resolved through the door
            // rather than read off the actor.
            REQUIRE(result.Ok());
            REQUIRE(result.point.x < 0.0f);
        }
    }
}
