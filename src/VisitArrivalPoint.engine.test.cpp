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
    // The shipped floor, not a convenient one. 2,000 is what makes a
    // directional test unnecessary: two hops reach a measured 1,784 units at
    // worst across all of vanilla, so every node hop expansion can get to on
    // the far side of the player is inside the floor and rejected on distance
    // before direction is ever a question. A fixture at 800 exercises a
    // configuration we do not ship, and is the only place an arrival could
    // land behind the player.
    constexpr const char* kSettings = "[Beats]\niVisitMarkerMinDistanceUnits=2000\n"
                                      "iVisitMarkerMaxDistanceUnits=5000\n"
                                      "iVisitArrivalCoverRadiusUnits=64\n"
                                      "bVisitArrivalAllowCoarseBearing=true\n"
                                      "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                      "bFineRoadsDebugBitmap=0\n"
                                      "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    constexpr std::uint32_t kWorld = 0x00D00001u;
    constexpr std::uint32_t kOtherWorld = 0x00D00002u;
    constexpr std::uint32_t kCityWorld = 0x00D00003u;
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
    // 500 units out to 7000 either side. Deliberately symmetric: which half of
    // it the search walks is decided only by where the visitor lives.
    //
    // Long enough that the shipped 2,000-unit floor still leaves several nodes
    // beyond it to rank. At seven nodes a side the floor landed on the
    // second-to-last one and there was nothing left to choose between.
    constexpr float kNodeSpacing = 500.0f;
    constexpr int kNodesPerSide = 14;
    constexpr float kRoadEnd = kNodeSpacing * kNodesPerSide; // 7000

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

    // Attach the 5x5 block of cells the engine loads at the default
    // uGridsToLoad, centred on the cell `roadCell` sits in, and keep the road
    // cell itself as the one carrying navmesh.
    //
    // Loading the single road cell instead — as this suite used to — puts the
    // grid at one cell across, so every position with a negative coordinate
    // falls OUTSIDE it. The arrival search then treats half the road as
    // unloaded ground and accepts it on distance alone, skipping the navmesh
    // and cover gates that several cases here exist to check. The real engine
    // never presents a one-cell grid, so neither should the fixture.
    void AttachGridAround(EngineMock& engine, RE::TESWorldSpace* space, RE::TESObjectCELL* roadCell)
    {
        std::vector<RE::TESObjectCELL*> cells{roadCell};
        for (std::int16_t cx = -2; cx <= 2; ++cx) {
            for (std::int16_t cy = -2; cy <= 2; ++cy) {
                if (cx == 0 && cy == 0) {
                    continue; // the road cell, already in
                }
                cells.push_back(engine.AddExteriorCell(space, cx, cy, nullptr));
            }
        }
        engine.LoadGrid(cells);
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
    AttachGridAround(engine, space, cell);
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
            REQUIRE(result.tier == VisitArrivalPoint::Tier::Chain);
            REQUIRE(std::fabs(result.point.y) < 1.0f);
        }

        SECTION("should take the nearest usable node rather than the farthest")
        {
            REQUIRE(result.Ok());
            // Nodes sit every 500 units, so the first at or past the shipped
            // 2,000-unit floor is the one at 2000 exactly. Everything nearer
            // is refused on distance -- including anything hop expansion
            // could reach on the far side of the player, which is why no
            // directional test is needed here.
            REQUIRE(std::fabs(result.point.x - 2000.0f) < 1.0f);
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
            REQUIRE(std::fabs(result.point.x + 2000.0f) < 1.0f);
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
    AttachGridAround(engine, space, cell);
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

TEST_CASE("VisitArrivalPoint takes the nearest point far enough out", "[VisitArrivalPoint][engine]")
{
    // The minimum distance is a rule; the maximum is not, and since Phase 16
    // it is not consulted on the road path at all. What replaced the band is
    // simpler and gives better answers: walk outward from the player and take
    // the FIRST point that is far enough away and hidden. Where the nearest
    // acceptable point sits inside the old ceiling the two agree, which is
    // what this fixture is -- the difference shows up in what becomes
    // fallback supply rather than in what wins.
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
    AttachGridAround(engine, space, cell);
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

    SECTION("should land on the nearest node past the minimum")
    {
        // 1500 is the first node outside the 1200-unit floor. Nothing further
        // out may win while it is usable, because a visitor who could have
        // arrived at 1500 should not be put at 3500 for the search's
        // convenience.
        REQUIRE(result.Ok());
        REQUIRE(std::fabs(result.point.x - 1500.0f) < 1.0f);
    }

    SECTION("should keep the road beyond the old ceiling as fallback supply")
    {
        // The old band held these back in a second pool and used them only if
        // the first was empty; before that it rejected them outright. They are
        // ordinary candidates now, and every one of them is further out than
        // the winner, which is what the escort needs to walk a stalled visitor
        // back along.
        REQUIRE(result.Ok());
        REQUIRE_FALSE(result.fallbacks.empty());
        const float winner = Dist2D(result.point, At(0.0f, 0.0f));
        for (const auto& fallback : result.fallbacks) {
            REQUIRE(Dist2D(fallback, At(0.0f, 0.0f)) > winner);
        }
    }

    SECTION("should report which class of point it stood the visitor on")
    {
        // Real road inside the loaded grid, which is rank 1 and the best
        // answer available. The two road tiers this replaces could only say
        // which generator had succeeded.
        REQUIRE(result.Ok());
        REQUIRE(result.tier == VisitArrivalPoint::Tier::Chain);
        REQUIRE(result.pointClass == NarrativeEngine::ApproachChain::PointClass::Fine);
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
    AttachGridAround(engine, space, cell);
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
            // Reaching further out is what happens when nothing nearer works,
            // not a preference: a visitor who could have arrived at the floor
            // should not be put at 7000 because the search liked it better.
            REQUIRE(result.Ok());
            REQUIRE(std::fabs(Dist2D(result.point, At(0.0f, 0.0f)) - 2000.0f) < 1.0f);
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
    AttachGridAround(engine, space, cell);
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
            // Open ground is a concession to where the player happens to be
            // looking, so it is held to a much longer distance than the
            // arrival floor: far enough out that a figure appearing is small
            // and ambiguous even if they turn a moment later. The first road
            // node at or past that distance is the one at 5000.
            REQUIRE(result.Ok());
            REQUIRE(std::fabs(Dist2D(result.point, At(0.0f, 0.0f)) - 5000.0f) < 1.0f);
        }

        SECTION("should refuse open ground nearer than that")
        {
            // Between the 2,000-unit floor and the open-ground distance there
            // are five usable road nodes, and every one of them is passed
            // over. Accepting the nearest would be treating "the player is
            // not looking" as equivalent to cover.
            REQUIRE(result.Ok());
            REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) > 2000.0f);
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

TEST_CASE("VisitArrivalPoint prefers cover to open ground", "[VisitArrivalPoint][engine]")
{
    // Open ground the player is not facing is a fallback, not an equal. It
    // depends on where somebody happens to be looking, and a player who turns
    // is a player who watched the arrival; real geometry does not care which
    // way they face.
    //
    // So the whole of the covered pool is considered before any of the open
    // pool, even though the outward walk reaches the open ground first. The
    // road here is exposed except for one patch of cover further out than the
    // nearest acceptable open node.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    AttachGridAround(engine, space, cell);
    PollFineRoads();
    LayGround(engine);

    // Exposed everywhere, except a rock beside the node at 6000.
    constexpr float kCoveredX = 6000.0f;
    engine.visibility.pickHitFraction = 1.0f;
    engine.visibility.coverPatches.push_back({kCoveredX, 0.0f, 300.0f, 0.0f});

    // Facing west, so the eastern road behind them is out of view and its
    // nodes from 5000 out are acceptable as open ground.
    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
    player->data.angle.z = 3.0f * 3.14159265f / 2.0f;
    auto* sender = ActorAt(engine, kSender, cell, At(kRoadEnd, 0.0f));
    const auto result = FindFor(sender, player);

    SECTION("when cover sits further out than usable open ground")
    {
        SECTION("should take the cover and walk past the open ground")
        {
            // 5000 and 5500 are both acceptable open ground and both nearer.
            // The covered node at 6000 wins anyway.
            REQUIRE(result.Ok());
            REQUIRE(std::fabs(result.point.x - kCoveredX) < 1.0f);
        }

        SECTION("should still keep the open ground as escort supply")
        {
            // A fallback is only ever reached once the visitor is stuck, and
            // every grade that got this far is one the player is not
            // watching. Throwing the rest away to keep the ladder tidy would
            // leave the escort with less to work with for no gain.
            REQUIRE(result.Ok());
            REQUIRE_FALSE(result.fallbacks.empty());
        }
    }

    SECTION("when the cover is removed")
    {
        engine.visibility.coverPatches.clear();
        const auto exposed = FindFor(sender, player);

        SECTION("should fall back to the nearest usable open ground")
        {
            // The same world with nothing to hide behind: now the open pool
            // is the best there is, and the nearest member of it wins. This
            // is the pair that says the case above was a preference rather
            // than a distance accident.
            REQUIRE(exposed.Ok());
            REQUIRE(std::fabs(Dist2D(exposed.point, At(0.0f, 0.0f)) - 5000.0f) < 1.0f);
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
    AttachGridAround(engine, space, cell);
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

TEST_CASE("VisitArrivalPoint expands off the road for a candidate", "[VisitArrivalPoint][engine]")
{
    // The fine graph is a road ribbon, so a node one hop off the chain is
    // usually the other side of the same road and two hops is a few metres
    // off it. That is where cheap cover lives when the road itself is in
    // plain view, and reaching it is the difference between a placed visit
    // and a declined one.
    //
    // Isolating it needs a world where the chain's own points cannot win. The
    // road here is short — every node on it sits inside the minimum distance,
    // so none of them is far enough to arrive at — and a two-node spur runs
    // north off the player's own node, out past that minimum. The spur is a
    // dead end, so no route ever passes through it: the only way to reach it
    // is to expand off the chain.
    constexpr float kFloor = 1200.0f;
    constexpr const char* kShortBand = "[Beats]\niVisitMarkerMinDistanceUnits=1200\n"
                                       "iVisitMarkerMaxDistanceUnits=2500\n"
                                       "iVisitArrivalCoverRadiusUnits=64\n"
                                       "bVisitArrivalAllowCoarseBearing=false\n"
                                       "iVisitChainHopRadius=2\n"
                                       "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                       "bFineRoadsDebugBitmap=0\n"
                                       "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    constexpr float kSpurNearY = 700.0f;
    constexpr float kSpurFarY = 1400.0f;

    // Indices into the mesh below: a five-node road, then the spur.
    //   0:(-1000,0) 1:(-500,0) 2:(0,0) 3:(500,0) 4:(1000,0) 5:(0,700) 6:(0,1400)
    const auto layShortRoadAndSpur = [](EngineMock& engine, RE::TESObjectCELL* cell) {
        std::vector<Triangle> mesh;
        for (int i = 0; i < 5; ++i) {
            Triangle t;
            t.x = -1000.0f + static_cast<float>(i) * 500.0f;
            t.y = 0.0f;
            t.z = kGround;
            t.neighbor[0] = i > 0 ? i - 1 : -1;
            t.neighbor[1] = i < 4 ? i + 1 : -1;
            mesh.push_back(t);
        }
        mesh[2].neighbor[2] = 5; // the spur leaves the player's own node
        Triangle spurNear;
        spurNear.x = 0.0f;
        spurNear.y = kSpurNearY;
        spurNear.z = kGround;
        spurNear.neighbor[0] = 2;
        spurNear.neighbor[1] = 6;
        mesh.push_back(spurNear);
        Triangle spurFar;
        spurFar.x = 0.0f;
        spurFar.y = kSpurFarY;
        spurFar.z = kGround;
        spurFar.neighbor[0] = 5;
        mesh.push_back(spurFar);
        engine.AddNavMesh(cell, kRoadMesh, mesh);
    };

    SECTION("when the only point far enough out is two hops off the chain")
    {
        EngineMock engine;
        const ConfiguredSettings settings{kShortBand};
        auto* space = engine.AddWorldSpace(kWorld);
        auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
        layShortRoadAndSpur(engine, cell);
        AttachGridAround(engine, space, cell);
        PollFineRoads();
        LayGround(engine);
        CoverEverywhere(engine);
        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        auto* sender = ActorAt(engine, kSender, cell, At(1000.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should stand the visitor on it")
        {
            REQUIRE(result.Ok());
            REQUIRE(std::fabs(result.point.y - kSpurFarY) < 1.0f);
            REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) >= kFloor);
        }

        SECTION("should still call it a fine point")
        {
            // A hop-expanded node is real road the engine has loaded, so it
            // gets the same gates and the same class as an on-chain one. Only
            // its rank differs, and rank only breaks ties.
            REQUIRE(result.Ok());
            REQUIRE(result.pointClass == NarrativeEngine::ApproachChain::PointClass::Fine);
        }
    }

    SECTION("when the expansion is switched off")
    {
        EngineMock engine;
        const ConfiguredSettings settings{"[Beats]\niVisitMarkerMinDistanceUnits=1200\n"
                                          "iVisitMarkerMaxDistanceUnits=2500\n"
                                          "iVisitArrivalCoverRadiusUnits=64\n"
                                          "bVisitArrivalAllowCoarseBearing=false\n"
                                          "iVisitChainHopRadius=0\n"
                                          "[FineRoads]\nbFineRoadsEnabled=1\n"
                                          "iFineRoadsBackstopSeconds=1\nbFineRoadsDebugBitmap=0\n"
                                          "[TravelGraph]\nbTravelGraphEnabled=1\n"
                                          "bTravelGraphDebugBitmap=0\n"};
        auto* space = engine.AddWorldSpace(kWorld);
        auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
        layShortRoadAndSpur(engine, cell);
        AttachGridAround(engine, space, cell);
        PollFineRoads();
        LayGround(engine);
        CoverEverywhere(engine);
        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        auto* sender = ActorAt(engine, kSender, cell, At(1000.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should not reach the spur at all")
        {
            // A radius of 0 is the on-chain-only search. Nothing on the chain
            // is far enough out, and the spur is not on it, so the visit
            // declines -- which is what says the win above came from the
            // expansion rather than from the spur being on the route.
            REQUIRE_FALSE(result.Ok());
        }
    }
}

TEST_CASE("VisitArrivalPoint checks synthetic ground inside the grid", "[VisitArrivalPoint][engine]")
{
    // A synthetic point is gated by WHERE IT IS, not by which line produced
    // it. Inside the loaded grid it can be navmesh-checked and cover-tested
    // like any road node, and it must be: waving it through for being
    // synthetic would place an actor on ground nobody looked at, while
    // refusing it for being synthetic would throw away the one part of each
    // direct line that can actually be verified.
    //
    // No fine graph and no coarse graph here, so every chain point except the
    // two endpoints is synthetic.
    constexpr const char* kNoRoads = "[Beats]\niVisitMarkerMinDistanceUnits=2000\n"
                                     "iVisitMarkerMaxDistanceUnits=5000\n"
                                     "iVisitArrivalCoverRadiusUnits=64\n"
                                     "bVisitArrivalAllowCoarseBearing=false\n"
                                     "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                     "bFineRoadsDebugBitmap=0\n"
                                     "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    SECTION("when the ground under it is walkable")
    {
        EngineMock engine;
        const ConfiguredSettings settings{kNoRoads};
        auto* space = engine.AddWorldSpace(kWorld);
        auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
        AttachGridAround(engine, space, cell);
        PollFineRoads();
        LayGround(engine);
        CoverEverywhere(engine);
        REQUIRE(FineRoads::NodeCount() == 0);

        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        auto* sender = ActorAt(engine, kSender, cell, At(4000.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should use it and say it was synthetic")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::Chain);
            REQUIRE(result.pointClass == NarrativeEngine::ApproachChain::PointClass::Direct);
        }

        SECTION("should keep it on the line home")
        {
            // The direct line runs outward from the visitor, so a point on it
            // is on the bearing home by construction.
            REQUIRE(result.Ok());
            REQUIRE(result.point.x > 0.0f);
            REQUIRE(std::fabs(result.point.y) < 1.0f);
        }
    }

    SECTION("when it is exposed and nearer than the open-ground distance")
    {
        EngineMock engine;
        const ConfiguredSettings settings{kNoRoads};
        auto* space = engine.AddWorldSpace(kWorld);
        auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
        AttachGridAround(engine, space, cell);
        PollFineRoads();
        LayGround(engine);
        // Walkable, but nothing to hide behind anywhere.
        engine.visibility.pickHitFraction = 1.0f;
        REQUIRE(FineRoads::NodeCount() == 0);

        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        auto* sender = ActorAt(engine, kSender, cell, At(4000.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should refuse it the way it would refuse exposed road")
        {
            // The distinguishing case for how a direct-line point is graded.
            // Every candidate here is synthetic, inside the loaded grid, on
            // navmesh, and between the 2,000-unit floor and the 5,000-unit
            // open-ground distance -- so cover is the only thing that could
            // accept one, and there is none.
            //
            // A point graded on its CLASS rather than its position would be
            // accepted on distance alone, because that is what "outside the
            // grid, nothing to test" means. These are inside the grid and
            // there is plenty to test, so the visit declines instead.
            REQUIRE_FALSE(result.Ok());
        }
    }

    SECTION("when it is exposed but beyond the open-ground distance")
    {
        EngineMock engine;
        const ConfiguredSettings settings{kNoRoads};
        auto* space = engine.AddWorldSpace(kWorld);
        auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
        AttachGridAround(engine, space, cell);
        PollFineRoads();
        LayGround(engine);
        engine.visibility.pickHitFraction = 1.0f;

        // Facing west, with the visitor away to the east.
        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        player->data.angle.z = 3.0f * 3.14159265f / 2.0f;
        auto* sender = ActorAt(engine, kSender, cell, At(9000.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should use it as unwatched open ground, not as unverified")
        {
            // With no road graph at all the line from the player to the
            // visitor is the whole chain, and the stretch of it inside the
            // loaded cells is as verifiable as any road: navmesh under it, a
            // walkable corridor home, and a cover test that can be asked.
            // So it earns the open-ground grade on the same terms a road node
            // would, rather than being written off for having come from a
            // synthetic line.
            REQUIRE(result.Ok());
            REQUIRE(result.pointClass == NarrativeEngine::ApproachChain::PointClass::Direct);
            REQUIRE(Dist2D(result.point, At(0.0f, 0.0f)) >= 5000.0f);
        }

        SECTION("should stand them on ground it actually checked")
        {
            // The claim "these are not unverified" in full: the winning point
            // is on navmesh the search re-read, not merely inside the grid.
            REQUIRE(result.Ok());
            RE::NiPoint3 grounded{};
            REQUIRE(StuckRecovery::IsStandable(result.point, grounded));
        }
    }

    SECTION("when there is no navmesh under it")
    {
        EngineMock engine;
        const ConfiguredSettings settings{kNoRoads};
        auto* space = engine.AddWorldSpace(kWorld);
        auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
        AttachGridAround(engine, space, cell);
        PollFineRoads();
        // Outdoors with a ground height, but no navmesh patch at all, so
        // every in-grid candidate fails the walkable check.
        engine.world.cellIsInterior = false;
        engine.terrain.landHeight = kGround;
        CoverEverywhere(engine);

        auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
        auto* sender = ActorAt(engine, kSender, cell, At(4000.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should refuse it rather than place an actor on it")
        {
            REQUIRE_FALSE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::None);
        }

        SECTION("should hand back no fallbacks either")
        {
            // A declined search that still supplied an escort ladder would
            // have the beat warping a visitor it never placed.
            REQUIRE(result.fallbacks.empty());
        }
    }
}

TEST_CASE("VisitArrivalPoint refuses road that has stopped being navmesh", "[VisitArrivalPoint][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    LayRoad(engine, cell);
    AttachGridAround(engine, space, cell);
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

TEST_CASE("VisitArrivalPoint still places a visitor with no fine road", "[VisitArrivalPoint][engine]")
{
    // No fine graph at all — the ordinary case indoors, on a fresh load, and
    // anywhere off the road network. Before Phase 16 this was a tier of its
    // own: manufacture a bearing from the coarse skeleton and sample an arc
    // around it. The chain needs no such thing, because its two direct lines
    // run outward from the visitor and one of them always reaches the player.
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
        REQUIRE(result.tier == VisitArrivalPoint::Tier::Chain);
        REQUIRE(result.point.x > playerPos.x);
    }

    SECTION("should stand them on the route rather than beside it")
    {
        // Both direct lines run from the visitor to a point on the road
        // network or to the player, so a point on either is on the bearing
        // home by construction. There is no arc to be off-centre of any more.
        REQUIRE(result.Ok());
        REQUIRE(std::fabs(result.point.y - playerPos.y) < 1.0f);
    }

    SECTION("should keep the minimum distance")
    {
        // The floor still applies. The ceiling does not, and must not: this
        // is exactly the shape of world where the only usable ground was
        // slightly too far out and the visit was declined over it.
        REQUIRE(result.Ok());
        REQUIRE(Dist2D(result.point, playerPos) >= 2000.0f);
    }
}

TEST_CASE("VisitArrivalPoint declines rather than place outside the loaded grid", "[VisitArrivalPoint][engine]")
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

    SECTION("should hold out for loaded ground rather than reach past it")
    {
        const auto result = FindFor(sender, player);
        // bVisitArrivalAllowCoarseBearing used to gate a bearing fallback that
        // no longer exists; it now asks the same question of the chain. False
        // confines every arrival to cells the engine has attached, where a
        // point can be navmesh-checked and cover-tested, and declines rather
        // than reaching past them. With no grid loaded here, that is every
        // candidate the chain has.
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
    AttachGridAround(engine, space, cell);
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
            // Every graph here is per-worldspace, and a plan across two of
            // them would be fiction. A walled city is joined to Skyrim by a
            // gate and gets its own treatment below; this grid holds no door
            // at all, which is Solstheim -- a boat, not a walk.
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
    AttachGridAround(engine, space, cell);
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
    AttachGridAround(engine, space, cell);
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

TEST_CASE("VisitArrivalPoint walks a city visitor in through the gate", "[VisitArrivalPoint][engine]")
{
    // Riften, Markarth, Solitude and Windhelm are each their own worldspace,
    // so a visitor who lives in Skyrim and a player standing in one of them
    // have coordinates that cannot be compared, let alone routed between.
    // Half the first broad in-game run died on exactly that. What joins the
    // two is the city gate: one door reference inside the walls, its partner
    // outside them.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* skyrim = engine.AddWorldSpace(kWorld);
    auto* city = engine.AddWorldSpace(kCityWorld);
    auto* outside = engine.AddExteriorCell(skyrim, 0, 0, nullptr);
    auto* cityCell = engine.AddExteriorCell(city, 0, 0, nullptr);
    engine.LoadGrid({cityCell});
    LayGround(engine);

    // The gate stands 2000 units east of the middle of the city and comes
    // out well into Skyrim.
    constexpr std::uint32_t kGate = 0x00D05001u;
    constexpr float kGateArrivalX = 6000.0f;
    auto* gate = engine.AddLoadDoor(cityCell, kGate, outside, At(kGateArrivalX, 0.0f));
    gate->data.location = At(2000.0f, 0.0f);

    // The visitor lives out in Skyrim, nowhere near the city's grid.
    auto* sender = ActorAt(engine, kSender, outside, At(20000.0f, 0.0f));

    SECTION("when the player is out in the streets with cover to spare")
    {
        CoverEverywhere(engine);
        auto* player = ActorAt(engine, kPlayer, cityCell, At(0.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should put the visitor inside the walls, between the player and the gate")
        {
            // Already through the gate is a shorter and more natural walk
            // than watching someone traverse it.
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::CityApproach);
            REQUIRE(result.point.x > 0.0f);
        }

        SECTION("should build the marker from the player, who is standing on that ground")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.placementAnchor == 0u);
        }
    }

    SECTION("when the player is watching the whole way to the gate")
    {
        // Open ground, and facing east -- which is where the gate is and so
        // where every candidate inside the walls would be.
        engine.visibility.pickHitFraction = 1.0f;
        auto* player = ActorAt(engine, kPlayer, cityCell, At(0.0f, 0.0f));
        player->data.angle.z = 3.14159265f / 2.0f;
        const auto result = FindFor(sender, player);

        SECTION("should place the visitor outside the gate and let them walk in")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::CityGate);
            REQUIRE(std::fabs(result.point.x - kGateArrivalX) < 1.0f);
            REQUIRE(std::fabs(result.point.z - kStandingZ) < 1.0f);
        }

        SECTION("should name the far door as what the marker is built from")
        {
            // PlaceObjectAtMe builds its reference in the CALLER's cell.
            // Placing from the player would put the marker inside the city
            // and land the visitor there, which is the whole thing this
            // tier exists to avoid. AddLoadDoor numbers the far side one
            // above the near one.
            REQUIRE(result.Ok());
            REQUIRE(result.placementAnchor == kGate + 1u);
        }
    }

    SECTION("when the player is standing at the gate")
    {
        // The failure this exists to stop: a player 1,168 units inside
        // Whiterun's gate got a visitor placed 1,151 units away and greeted
        // them 1.3 seconds after arming. "Outside the gate" is only an
        // arrival if the gate is far enough off to walk in from.
        // Facing the gate, so nothing between the player and it survives
        // and the search falls through to the gate itself -- which is 1,000
        // units off, under the floor.
        engine.visibility.pickHitFraction = 1.0f;
        auto* player = ActorAt(engine, kPlayer, cityCell, At(5000.0f, 0.0f));
        player->data.angle.z = 3.0f * 3.14159265f / 2.0f;
        const auto result = FindFor(sender, player);

        SECTION("should keep walking out until the floor is satisfied")
        {
            // The gate's own landing is 1,000 units off, under the 2,000
            // floor, so the arrival moves further out along the same line the
            // visitor walks in on.
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::CityGate);
            REQUIRE(Dist2D(result.point, At(5000.0f, 0.0f)) >= 2000.0f);
            REQUIRE(result.point.x > kGateArrivalX);
        }

        SECTION("should stay on the line through the gate")
        {
            // Pushed out along the player-to-gate bearing rather than in some
            // direction of its own, so the visitor still comes from where the
            // gate is.
            REQUIRE(result.Ok());
            REQUIRE(std::fabs(result.point.y) < 1.0f);
        }

        SECTION("should stand them on ground it checked")
        {
            REQUIRE(result.Ok());
            RE::NiPoint3 grounded{};
            REQUIRE(StuckRecovery::IsStandable(result.point, grounded));
        }

        SECTION("should keep the gate itself as escort supply")
        {
            // The one position in here the engine vouches for: it is where
            // anybody walking through the door lands.
            REQUIRE(result.Ok());
            REQUIRE_FALSE(result.fallbacks.empty());
            const auto& last = result.fallbacks.back();
            REQUIRE(std::fabs(last.x - kGateArrivalX) < 1.0f);
        }

        SECTION("should still name the far door as what the marker is built from")
        {
            // Pushing the point out does not change which worldspace it is
            // in, so the anchor is still the door on the far side.
            REQUIRE(result.Ok());
            REQUIRE(result.placementAnchor == kGate + 1u);
        }
    }

    SECTION("when the city has a second gate nearer the player")
    {
        CoverEverywhere(engine);
        constexpr std::uint32_t kWestGate = 0x00D05010u;
        auto* westGate = engine.AddLoadDoor(cityCell, kWestGate, outside, At(-9000.0f, 0.0f));
        westGate->data.location = At(-500.0f, 0.0f);
        auto* player = ActorAt(engine, kPlayer, cityCell, At(0.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should bring the visitor in through that one")
        {
            // Nearest wins. Sending someone to the far gate of a city they
            // are standing beside is a longer walk for no reason, and the
            // player watches all of it.
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::CityApproach);
            REQUIRE(result.point.x < 0.0f);
        }
    }

    SECTION("when the player is indoors in the city")
    {
        // The gate stops being the question. A city inn's load door already
        // names an exterior door, and the visitor waiting at it is both
        // simpler and better than sending them to the city gate to walk a
        // route the player is not on.
        CoverEverywhere(engine);
        auto* inn = engine.AddExteriorCell(city, 9, 9, nullptr);
        engine.SetCellInterior(inn, true);
        constexpr std::uint32_t kInnDoor = 0x00D05020u;
        constexpr float kDoorstepX = -3000.0f;
        engine.AddLoadDoor(inn, kInnDoor, cityCell, At(kDoorstepX, 0.0f));
        auto* player = ActorAt(engine, kPlayer, inn, At(50.0f, 50.0f));
        const auto result = FindFor(sender, player);

        SECTION("should wait at the inn's own door, not out by the gate")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::Doorstep);
            REQUIRE(result.point.x == kDoorstepX);
            REQUIRE(result.placementAnchor == kInnDoor + 1u);
        }
    }
}

TEST_CASE("VisitArrivalPoint still arrives when the road under the player is a stub", "[VisitArrivalPoint][engine]")
{
    // Rorikstead and Loreius Farm declined every visit, and the reason was
    // an interaction rather than a missing case. `Route` calls a destination
    // "within fine" when the nearest REACHABLE fine node is within two cells
    // of it -- and the fine graph is per-cell, so the component under the
    // player is often a stub of a few nodes while the rest of the road sits
    // in another component. The stub satisfies that test, so the plan comes
    // back as a fine-only path of three nodes with an EMPTY coarse path.
    //
    // Every one of those three was inside the minimum distance, and the
    // bearing fallback then had nothing to aim at, because it only ever read
    // the coarse path. The approach chain has no such gap: the coarse
    // skeleton and the stub are one graph, bridged, so the route carries on
    // past the stub instead of ending at it.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00D06000u;
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

    // The stub: three nodes, none of them far enough from the player to be
    // usable. Every fine candidate will be rejected as too near.
    const RE::NiPoint3 playerPos = At(2048.0f, 2048.0f);
    std::vector<Triangle> stub;
    for (int i = 0; i < 3; ++i) {
        Triangle t;
        t.x = playerPos.x + static_cast<float>(i) * 300.0f;
        t.y = playerPos.y;
        t.z = kGround;
        t.neighbor[0] = i > 0 ? i - 1 : -1;
        t.neighbor[1] = i < 2 ? i + 1 : -1;
        stub.push_back(t);
    }
    engine.AddNavMesh(cell, kRoadMesh, stub);
    AttachGridAround(engine, space, cell);
    PollFineRoads();
    REQUIRE(FineRoads::NodeCount() == 3);

    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, playerPos);
    auto* sender = ActorAt(engine, kSender, cell, At(20480.0f, 2048.0f));
    const auto result = FindFor(sender, player);

    SECTION("should fall back to the bearing rather than decline")
    {
        REQUIRE(result.Ok());
        REQUIRE(result.tier == VisitArrivalPoint::Tier::Chain);
    }

    SECTION("should still bring the visitor in from the direction of home")
    {
        REQUIRE(result.Ok());
        REQUIRE(result.point.x > playerPos.x);
    }

    SECTION("should keep the minimum distance")
    {
        // The floor still applies; the ceiling does not. On a stub the only
        // ground on the route home may be a whole cell out, and refusing it
        // over a walk being long is how this shape of world used to lose its
        // visits altogether.
        REQUIRE(result.Ok());
        REQUIRE(Dist2D(result.point, playerPos) >= 2000.0f);
    }
}

TEST_CASE("VisitArrivalPoint waits at the door when the player is indoors", "[VisitArrivalPoint][engine]")
{
    // Standing in an interior empties the exterior cell grid, so every gate
    // the search runs has nothing to read: `IsStandable` asks
    // `TES::GetLandHeight`, which has no landscape to answer from, and in
    // game all 45 bearing samples came back off-navmesh. There is no point
    // outdoors that can be validated while the player is inside.
    //
    // None of that needs solving, because the answer needs no validating.
    // The interior's load door names an exterior door, and the engine's own
    // arrival point for it is where the player is placed every time they
    // walk out. The visitor waits there.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    auto* space = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
    auto* inn = engine.AddExteriorCell(space, 9, 9, nullptr);
    engine.SetCellInterior(inn, true);

    constexpr std::uint32_t kInnDoor = 0x00D09001u;
    const RE::NiPoint3 doorstep = At(2048.0f, 2048.0f);
    engine.AddLoadDoor(inn, kInnDoor, cell, doorstep);

    // Interior coordinates, nowhere near the doorstep in world terms.
    auto* player = ActorAt(engine, kPlayer, inn, At(-20000.0f, -20000.0f));

    SECTION("when the visitor lives out in the same worldspace")
    {
        auto* sender = ActorAt(engine, kSender, cell, At(24576.0f, 2048.0f));
        const auto result = FindFor(sender, player);

        SECTION("should put them on the far side of the player's own door")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::Doorstep);
            REQUIRE(result.point.x == doorstep.x);
            REQUIRE(result.point.y == doorstep.y);
        }

        SECTION("should build the marker from that door rather than from the player")
        {
            // PlaceObjectAtMe works in the CALLER's cell, and the player is
            // in the wrong one -- building from them would put the visitor
            // in the room. AddLoadDoor numbers the far side one above.
            REQUIRE(result.Ok());
            REQUIRE(result.placementAnchor == kInnDoor + 1u);
        }

        SECTION("should not go looking for a road it cannot reach")
        {
            // No fine graph, no coarse graph and no ground were laid here,
            // which is the state the game is actually in with the player
            // indoors. A tier that depended on any of them would decline.
            REQUIRE(result.Ok());
        }
    }

    SECTION("when the visitor lives in another worldspace")
    {
        // A city inn. The door short-circuits the worldspace mismatch
        // entirely -- there is no gate to find, because the visitor is not
        // going through one.
        auto* elsewhere = engine.AddWorldSpace(kOtherWorld);
        auto* farCell = engine.AddExteriorCell(elsewhere, 0, 0, nullptr);
        auto* sender = ActorAt(engine, kSender, farCell, At(0.0f, 0.0f));
        const auto result = FindFor(sender, player);

        SECTION("should still wait at the player's door")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::Doorstep);
            REQUIRE(result.placementAnchor == kInnDoor + 1u);
        }
    }
}

TEST_CASE("VisitArrivalPoint never measures the road from interior coordinates", "[VisitArrivalPoint][engine]")
{
    // Some interiors genuinely have no load door out, and those fall back to
    // a map marker. The search then runs as normal -- and it must run from
    // the MARKER, not from the player's interior position, which is small
    // numbers local to the cell. Feeding those to the coarse graph picks
    // whatever node sits near the worldspace origin; in game that sent every
    // indoor dispatch chasing a corridor ten thousand units off.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00D0A000u;
    std::vector<const RE::BSNavmeshInfo*> road;
    for (int cellX = 0; cellX <= 6; ++cellX) {
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

    // The marker sits on the road's first node; home is far east along it.
    const RE::NiPoint3 marker = At(2048.0f, 2048.0f);
    auto* vault = engine.AddExteriorCell(space, 9, 9, nullptr);
    engine.SetCellInterior(vault, true);
    auto* location = engine.AddLocation(0x00D0A100u, "The Vault", {});
    engine.SetLocationMarker(location, cell, marker);
    engine.world.playerHasLocation = true;
    engine.world.playerLocationOverride = location;

    auto* player = ActorAt(engine, kPlayer, vault, At(-20000.0f, -20000.0f));
    auto* sender = ActorAt(engine, kSender, cell, At(26624.0f, 2048.0f));
    const auto result = FindFor(sender, player);

    SECTION("should bring the visitor in from the marker's side of the world")
    {
        // East of the marker, because that is where home is from there.
        // Measured from the player's own coordinates the whole search runs
        // twenty thousand units away in the opposite corner.
        REQUIRE(result.Ok());
        REQUIRE(result.point.x > marker.x);
        REQUIRE(Dist2D(result.point, marker) <= 8000.0f);
    }
}

TEST_CASE("VisitArrivalPoint does not arrive from the nearest scrap of road", "[VisitArrivalPoint][engine]")
{
    // The coarse graph holds one node per navmesh cell, so the node nearest
    // the player can sit most of a cell away in a direction decided by where
    // the road runs rather than by where the visitor lives. Aiming the old
    // fallback arc at it produced arrivals 115, 159 and 207 degrees off home
    // in one run.
    //
    // Phase 14 answered that with an approach corridor, which Phase 16
    // retires: the chain is a route FROM the visitor, so the points on it are
    // on the way home whether or not anything checks. This fixture is the
    // regression test for that claim rather than for the corridor.
    //
    // The road here runs due east to the visitor, but it starts with a spur
    // hanging SOUTH of the player, and that spur holds the node nearest to
    // them.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    auto* navi = engine.AddNavMeshInfoMap(kNavi);
    std::uint32_t nextForm = 0x00D08000u;
    std::vector<const RE::BSNavmeshInfo*> road;
    // The spur: one cell south of the player, and the closest node to them.
    road.push_back(engine.AddNavmeshInfo(navi, nextForm++, kWorld, 0, -1, 0.0f, 0.0f, 0.0f));
    for (int cellX = 0; cellX <= 6; ++cellX) {
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

    // Standing between the spur and the road east, nearer the spur, and with
    // no fine graph at all so the bearing is the only tier left.
    const RE::NiPoint3 playerPos = At(2048.0f, 700.0f);
    auto* player = ActorAt(engine, kPlayer, cell, playerPos);
    auto* sender = ActorAt(engine, kSender, cell, At(26624.0f, 2048.0f));
    REQUIRE(FineRoads::NodeCount() == 0);
    const auto result = FindFor(sender, player);

    SECTION("should come in from the road home rather than off the spur")
    {
        // The spur node is due south, and an arrival south of the player is
        // the bug. North is where the road is: the player is standing off it,
        // so the visitor comes in along it and reaches them from there. The
        // old assertion asked for EAST, which was the bearing arc's answer --
        // the chain's is the route, and the route runs down the road.
        REQUIRE(result.Ok());
        REQUIRE(result.tier == VisitArrivalPoint::Tier::Chain);
        REQUIRE(result.point.y > playerPos.y);
    }
}

TEST_CASE("VisitArrivalPoint walks a city visitor out to a player who is nowhere near it",
          "[VisitArrivalPoint][engine]")
{
    // The mirror of the gate case above, and the one every real dispatch hit.
    //
    // A visitor who lives inside Whiterun resolves to WhiterunWorld, a player
    // standing out in Eastmarch resolves to Tamriel, and the gate that joins
    // them is thousands of units away in a cell nothing has loaded. The gate
    // search can only see the grid around the player, so it finds nothing,
    // and for as long as that was the end of the ladder every sender behind a
    // set of walls declined -- five cities' worth of the people most worth
    // visiting.
    //
    // Nothing about the gate is needed to answer. What the route wants from
    // the visitor's end is a BEARING, and their home's map marker is one: it
    // stands in the city's own worldspace, but a city overlays the same
    // ground its parent does, so the marker's coordinates are already the
    // player's coordinates. Whiterun's marker reads (19855,-7422) and the
    // city's cells sit at grid (4,-2) of Tamriel, which is the same place.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* skyrim = engine.AddWorldSpace(kWorld);
    auto* city = engine.AddWorldSpace(kCityWorld);
    engine.SetWorldSpaceParent(city, skyrim);

    auto* cell = engine.AddExteriorCell(skyrim, 0, 0, nullptr);
    LayRoad(engine, cell);
    AttachGridAround(engine, skyrim, cell);
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));

    // The city: its own location, its own cell, and a marker of its own
    // standing inside its walls.
    auto* home = engine.AddLocation(0x00D06001u, "The City", {}, "CityLocation");
    auto* cityCell = engine.AddExteriorCell(city, 4, -2, home);

    SECTION("when the city they live in lies east")
    {
        engine.SetLocationMarker(home, cityCell, At(kRoadEnd, 0.0f));
        auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
        engine.SetSaveParentCell(sender, cityCell);
        const auto result = FindFor(sender, player);

        SECTION("should bring them in from the east rather than decline")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x > 0.0f);
        }

        SECTION("should route them onto the road, not merely somewhere eastward")
        {
            REQUIRE(result.tier == VisitArrivalPoint::Tier::Chain);
        }

        SECTION("should build the marker from the player, who shares that ground")
        {
            // Not a city tier: the arrival is in the player's own worldspace
            // and their own cell, so there is no door to anchor on and none
            // is wanted.
            REQUIRE(result.placementAnchor == 0u);
        }
    }

    SECTION("when the city they live in lies west")
    {
        engine.SetLocationMarker(home, cityCell, At(-kRoadEnd, 0.0f));
        auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
        engine.SetSaveParentCell(sender, cityCell);
        const auto result = FindFor(sender, player);

        SECTION("should mirror the eastern answer exactly")
        {
            // Same visitor, same cell, same coordinates inside the walls --
            // and the opposite arrival, because the marker is the only thing
            // being read.
            REQUIRE(result.Ok());
            REQUIRE(result.point.x < 0.0f);
        }
    }

    SECTION("when the city sits inside a hold marked somewhere else entirely")
    {
        // "Deepest" is the whole claim. The hold's marker would put a visitor
        // from the city on the road from wherever the hold happens to be
        // pinned, which is a worse answer than the city they actually live in
        // -- and the hold is what a climb finds if it does not stop at the
        // first marker it meets.
        auto* hold = engine.AddLocation(0x00D06002u, "The Hold", {}, "HoldLocation");
        engine.SetLocationParent(home, hold);
        engine.SetLocationMarker(hold, cell, At(-kRoadEnd, 0.0f));
        engine.SetLocationMarker(home, cityCell, At(kRoadEnd, 0.0f));

        auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
        engine.SetSaveParentCell(sender, cityCell);
        const auto result = FindFor(sender, player);

        SECTION("should come from the city rather than from the hold above it")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x > 0.0f);
        }
    }

    SECTION("when the street they stand on names no location, as a city street does not")
    {
        // The rung the real dispatch needed. Not one of WhiterunWorld's 113
        // exterior cells fills XLCN -- the field is set once, on the
        // worldspace, and reads WhiterunLocation there. Asking only the cell
        // answers for the visitor indoors and for nobody out in the street,
        // which is the wrong half of the city.
        auto* streets = engine.AddExteriorCell(city, 5, -2, nullptr);
        engine.SetWorldSpaceLocation(city, home);
        engine.SetLocationMarker(home, cityCell, At(kRoadEnd, 0.0f));

        auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
        engine.SetSaveParentCell(sender, streets);
        const auto result = FindFor(sender, player);

        SECTION("should read the city off the worldspace and still bring them in")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.point.x > 0.0f);
        }
    }

    SECTION("when a gate into the city is loaded after all")
    {
        // The player standing at the gate is the case the city tiers were
        // built for, and they are still the better answer there: a real door
        // beats a marker read off a record. The marker rung is a fallback
        // from the gate search, not a replacement for it.
        engine.SetLocationMarker(home, cityCell, At(kRoadEnd, 0.0f));
        constexpr std::uint32_t kGate = 0x00D06010u;
        auto* gate = engine.AddLoadDoor(cell, kGate, cityCell, At(-2000.0f, 0.0f));
        gate->data.location = At(-1500.0f, 0.0f);

        auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
        engine.SetSaveParentCell(sender, cityCell);
        const auto result = FindFor(sender, player);

        SECTION("should use the gate rather than the home marker")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.tier == VisitArrivalPoint::Tier::CityApproach);
            // Toward the gate, which is west -- the opposite side from the
            // marker, so the two cannot be confused for one another.
            REQUIRE(result.point.x < 0.0f);
        }
    }
}

TEST_CASE("VisitArrivalPoint will not read a marker out of ground the player does not stand on",
          "[VisitArrivalPoint][engine]")
{
    // The limit on the rung above. A marker is a usable bearing only where its
    // worldspace and the player's measure from the same origin, which is what
    // a shared parent chain means. Without one the numbers stop being
    // comparable: Solstheim's (19855,-7422) and Tamriel's are different places
    // written the same way, and routing between them would put the visitor on
    // a bearing built out of nothing.
    //
    // That is the failure this module exists to prevent, so it declines --
    // which for Solstheim is also just true. It is a boat, not a walk.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* skyrim = engine.AddWorldSpace(kWorld);
    auto* cell = engine.AddExteriorCell(skyrim, 0, 0, nullptr);
    LayRoad(engine, cell);
    AttachGridAround(engine, skyrim, cell);
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);

    auto* player = ActorAt(engine, kPlayer, cell, At(0.0f, 0.0f));
    auto* home = engine.AddLocation(0x00D07001u, "Far Shore", {}, "FarShoreLocation");

    SECTION("when their island answers to no parent at all")
    {
        auto* island = engine.AddWorldSpace(kOtherWorld);
        auto* islandCell = engine.AddExteriorCell(island, 0, 0, home);
        engine.SetLocationMarker(home, islandCell, At(kRoadEnd, 0.0f));
        auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
        engine.SetSaveParentCell(sender, islandCell);

        SECTION("should decline rather than invent a bearing out of it")
        {
            REQUIRE_FALSE(FindFor(sender, player).Ok());
        }
    }

    SECTION("when their realm hangs off that island rather than off Skyrim")
    {
        // Apocrypha's parent is Solstheim, so climbing it arrives at a root
        // the player has never stood in. Having A parent is not the test --
        // arriving at the SAME one is.
        auto* island = engine.AddWorldSpace(kOtherWorld);
        auto* realm = engine.AddWorldSpace(0x00D00005u);
        engine.SetWorldSpaceParent(realm, island);
        auto* realmCell = engine.AddExteriorCell(realm, 0, 0, home);
        engine.SetLocationMarker(home, realmCell, At(kRoadEnd, 0.0f));
        auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
        engine.SetSaveParentCell(sender, realmCell);

        SECTION("should still decline")
        {
            REQUIRE_FALSE(FindFor(sender, player).Ok());
        }
    }
}

TEST_CASE("VisitArrivalPoint compares two cities that measure from the same origin", "[VisitArrivalPoint][engine]")
{
    // Neither end is standing in Tamriel, and they still compare. Two walled
    // cities that each hang under Skyrim measure from Skyrim's origin and so
    // from each other's, which makes a player in one and a visitor living in
    // the other a real bearing that declining would throw away.
    //
    // A worldspace being "the player's own" is therefore not the test.
    // Arriving at the same root is, and that is what the check is written
    // against.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* skyrim = engine.AddWorldSpace(kWorld);
    auto* ourCity = engine.AddWorldSpace(kCityWorld);
    auto* theirCity = engine.AddWorldSpace(0x00D00004u);
    engine.SetWorldSpaceParent(ourCity, skyrim);
    engine.SetWorldSpaceParent(theirCity, skyrim);

    // The road, and the player on it, are inside the city the player is in.
    auto* ourCell = engine.AddExteriorCell(ourCity, 0, 0, nullptr);
    LayRoad(engine, ourCell);
    engine.LoadGrid({ourCell});
    PollFineRoads();
    LayGround(engine);
    CoverEverywhere(engine);
    auto* player = ActorAt(engine, kPlayer, ourCell, At(0.0f, 0.0f));

    auto* home = engine.AddLocation(0x00D08001u, "The Other City", {}, "OtherCityLocation");
    auto* theirCell = engine.AddExteriorCell(theirCity, 4, -2, home);
    engine.SetLocationMarker(home, theirCell, At(kRoadEnd, 0.0f));
    auto* sender = ActorAt(engine, kSender, nullptr, At(100.0f, 100.0f));
    engine.SetSaveParentCell(sender, theirCell);

    SECTION("should bring the visitor in from the side their city is on")
    {
        const auto result = FindFor(sender, player);
        REQUIRE(result.Ok());
        REQUIRE(result.point.x > 0.0f);
    }
}
