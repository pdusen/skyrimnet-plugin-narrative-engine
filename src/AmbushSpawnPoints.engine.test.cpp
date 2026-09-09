#include <AmbushSpawnPoints.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <StuckRecovery.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Tests for where an ambush comes from.
//
// The question is "where can three bandits appear so that the player walks into
// them", and almost every way of getting it wrong is invisible from the inside.
// Spawn somewhere the player is looking and they watch the encounter be
// conjured. Spawn on a ledge and the attackers stand there for the whole beat.
// Spawn in a lake and they swim at you. Refuse to spawn at all and the Director
// simply goes quiet in whole regions, which reads exactly like a mod that is
// not working.
//
// So the search is a stack of gates over a ring of sampled points, and the
// cases below are mostly about which gate rejects what — plus the two rankings
// that decide the winner once the gates are done.
//
// Those two rankings are opposites, deliberately. When cover exists, the
// forward arc beats the rear as a hard tier: an ambush wants to be walked into.
// When nothing anywhere is hidden — an open plain, which is a large fraction of
// Skyrim — the only thing left to hide behind is the player's own back, so the
// fallback tier prefers the rear and the farthest point it can find. A test
// that only covered the first would let the second silently invert.
//
// The engine side is a flat world whose ground height, water and cover are each
// one switch, plus a navmesh laid as real triangles — because containment is
// tested against real triangle data, and a patch is how a test says where an
// actor may stand.

namespace
{
    namespace AmbushSpawnPoints = NarrativeEngine::AmbushSpawnPoints;
    namespace StuckRecovery = NarrativeEngine::StuckRecovery;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr const char* kSettings = "[Beats]\niAmbushMinSpawnDistanceUnits=2500\n"
                                      "iAmbushMaxSpawnDistanceUnits=5500\n";

    constexpr std::uint32_t kPlayer = 0x00000014u;
    constexpr std::uint32_t kGroundMesh = 0x00C00001u;

    // Wide enough to hold every ring the search widens through.
    constexpr float kWorldEdge = 9000.0f;

    // Ground is flat at zero and the search stands actors just clear of it.
    constexpr float kGround = 0.0f;
    constexpr float kStandingZ = kGround + StuckRecovery::kGroundClearanceUnits;

    // A player at the origin facing north, which is where a Z angle of zero
    // points: Skyrim measures it clockwise from +Y.
    RE::Actor* PlayerFacingNorth(EngineMock& engine)
    {
        auto* player = engine.AddActor(kPlayer);
        player->data.location = RE::NiPoint3{0.0f, 0.0f, kGround};
        player->data.angle = RE::NiPoint3{0.0f, 0.0f, 0.0f};
        return player;
    }

    float DistanceFromPlayer(const RE::NiPoint3& p)
    {
        return std::sqrt(p.x * p.x + p.y * p.y);
    }

    // Positive is in front of a player facing north.
    float Aheadness(const RE::NiPoint3& p)
    {
        return p.y;
    }
} // namespace

TEST_CASE("AmbushSpawnPoints hides attackers in front of the player", "[AmbushSpawnPoints][engine]")
{
    // Happy path, re-run per leaf: flat dry ground with navmesh over all of it,
    // and every line from the camera blocked — so cover exists everywhere and
    // the only thing left to decide is the ranking.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, kWorldEdge, kGround);
    engine.visibility.pickHitFraction = 0.0f;

    SECTION("when the ground all round is usable")
    {
        const auto result = AmbushSpawnPoints::Find(player, 2500, 3);

        SECTION("should place one attacker per body asked for")
        {
            REQUIRE(result.Ok());
            REQUIRE(result.spawnPoints.size() == 3);
        }

        SECTION("should put them ahead of the player rather than behind")
        {
            REQUIRE(result.Ok());
            // The whole shape of the beat. An ambush the player walks into
            // reads as an ambush; one that appears at their back reads as a
            // spawn, and there is nothing the beat can do afterwards to
            // recover the impression.
            REQUIRE(Aheadness(result.spawnPoints.front()) > 0.0f);
        }

        SECTION("should keep them near the distance asked for")
        {
            REQUIRE(result.Ok());
            REQUIRE(std::fabs(DistanceFromPlayer(result.spawnPoints.front()) - 2500.0f) < 50.0f);
        }

        SECTION("should stand them clear of the ground")
        {
            REQUIRE(result.Ok());
            // Placed exactly on the surface, an actor drops through it or
            // wedges in it depending on the frame it lands on.
            REQUIRE(std::fabs(result.spawnPoints.front().z - kStandingZ) < 1.0f);
        }

        SECTION("should spread them out rather than stack them")
        {
            REQUIRE(result.spawnPoints.size() == 3);
            // Three actors on one coordinate shove each other apart on the
            // first physics frame, which is visible from a long way off.
            REQUIRE(result.spawnPoints[1].GetDistance(result.spawnPoints[0]) > 1.0f);
            REQUIRE(result.spawnPoints[2].GetDistance(result.spawnPoints[1]) > 1.0f);
        }

        SECTION("should keep runner-ups for the recovery to use")
        {
            REQUIRE_FALSE(result.fallbacks.empty());
        }

        SECTION("should make each runner-up somewhere genuinely else")
        {
            // Terrain an actor could not travel out of does not improve fifty
            // units along, so runner-ups that close are one option reported
            // several times — and the recovery would work through all of them
            // and land back where it started.
            REQUIRE(result.Ok());
            for (std::size_t i = 0; i < result.fallbacks.size(); ++i) {
                REQUIRE(result.fallbacks[i].GetDistance(result.spawnPoints.front()) >= 800.0f);
                for (std::size_t j = i + 1; j < result.fallbacks.size(); ++j) {
                    REQUIRE(result.fallbacks[i].GetDistance(result.fallbacks[j]) >= 800.0f);
                }
            }
        }
    }
}

TEST_CASE("AmbushSpawnPoints takes the rear when the front is unusable", "[AmbushSpawnPoints][engine]")
{
    // Navmesh over the southern half only — a cliff, a lake, a chasm, anything
    // that leaves the ground in front of the player unstandable at every
    // radius. Cover exists, so this is the covered ranking falling through
    // from its preferred tier to its second one rather than the open-plain
    // fallback.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, -100.0f, kGround);
    engine.visibility.pickHitFraction = 0.0f;
    const auto result = AmbushSpawnPoints::Find(player, 2500, 1);

    SECTION("when nothing in front can be stood on")
    {
        SECTION("should place them behind rather than refuse")
        {
            // Refusing here would mean no ambush ever happens on ground with a
            // cliff on one side, which is a great deal of the province.
            REQUIRE(result.Ok());
            REQUIRE(Aheadness(result.spawnPoints.front()) < 0.0f);
        }
    }
}

TEST_CASE("AmbushSpawnPoints prefers the front even when the rear scores better", "[AmbushSpawnPoints][engine]")
{
    // Two patches of standable ground and nothing else: one directly ahead at
    // the far edge of the band, and one just off the player's shoulder, barely
    // into the rear hemisphere and right at the near edge.
    //
    // The placement score weighs angle and distance equally, so the near one
    // scores better — a candidate a few degrees behind at 2500 units beats one
    // dead ahead at 5500. The forward arc is a hard tier above that score for
    // exactly this reason: an ambush the player turns round and finds is not an
    // ambush, however well placed it is. Without the tier this case walks the
    // attackers in behind them.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.visibility.pickHitFraction = 0.0f;
    // Dead ahead, on the outermost ring the search widens to.
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -300.0f, 5300.0f, 300.0f, 5700.0f, kGround);
    // Off the shoulder, one azimuth step past square, on the innermost ring.
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh + 1u, 2300.0f, -600.0f, 2600.0f, -350.0f, kGround);
    const auto result = AmbushSpawnPoints::Find(player, 2500, 1);

    SECTION("when the only near ground is behind the player")
    {
        SECTION("should walk out to the far ground in front instead")
        {
            REQUIRE(result.Ok());
            REQUIRE(Aheadness(result.spawnPoints.front()) > 0.0f);
        }
    }
}

TEST_CASE("AmbushSpawnPoints falls back to open ground", "[AmbushSpawnPoints][engine]")
{
    // Flat dry navmeshed ground again, but every ray from the camera reaches
    // its endpoint: nothing anywhere is hidden. That is an open plain, and it
    // is a large fraction of Skyrim.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, kWorldEdge, kGround);
    engine.visibility.pickHitFraction = 1.0f;
    const auto result = AmbushSpawnPoints::Find(player, 2500, 2);

    SECTION("when no cover exists anywhere")
    {
        SECTION("should still place the attackers")
        {
            // Declining would mean whole regions where an ambush can never
            // happen, and nothing in the log to say why.
            REQUIRE(result.Ok());
        }

        SECTION("should put them behind the player instead")
        {
            REQUIRE(result.Ok());
            // With nothing to hide behind, the player's own back is the only
            // cover left. This inverts the ranking used when cover exists,
            // which is why it is worth stating out loud.
            REQUIRE(Aheadness(result.spawnPoints.front()) < 0.0f);
        }

        SECTION("should take the farthest point it found")
        {
            REQUIRE(result.Ok());
            // Distance is the only remaining way to buy time before they are
            // seen, so the search keeps widening and takes the outer ring.
            REQUIRE(DistanceFromPlayer(result.spawnPoints.front()) > 5000.0f);
        }
    }
}

TEST_CASE("AmbushSpawnPoints refuses ground nobody could stand on", "[AmbushSpawnPoints][engine]")
{
    // Every leaf starts from usable ground and then takes one thing away, so
    // the empty result names the gate that did it.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, kWorldEdge, kGround);
    engine.visibility.pickHitFraction = 0.0f;

    SECTION("when the surrounding cells are not loaded")
    {
        engine.terrain.cellPresent = false;

        SECTION("should find nowhere")
        {
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 3).Ok());
        }
    }

    SECTION("when the player is indoors")
    {
        engine.world.cellIsInterior = true;

        SECTION("should find nowhere")
        {
            // The beat spawns a travelling approach. An interior has neither
            // the room for one nor the sightlines to read it.
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 3).Ok());
        }
    }

    SECTION("when there is no ground to stand on")
    {
        engine.terrain.landHeightResolves = false;

        SECTION("should find nowhere")
        {
            // Off the edge of the world, which the ring reaches wherever the
            // player is near a border.
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 3).Ok());
        }
    }

    SECTION("when the ground is a long way below the player")
    {
        engine.terrain.landHeight = kGround - 4000.0f;

        SECTION("should find nowhere")
        {
            // Standing in for reachability, which there is no engine query to
            // ask: the navmesh test says a point has ground under it, not that
            // a route to it exists, and unreachable ground is almost always a
            // long way up or down.
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 3).Ok());
        }
    }

    SECTION("when the ground is under water")
    {
        engine.terrain.hasWater = true;
        engine.terrain.waterHeight = kGround + 200.0f;

        SECTION("should find nowhere")
        {
            // Lake and river beds carry navmesh like anywhere else, so
            // containment alone accepts them and the attackers arrive
            // swimming.
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 3).Ok());
        }
    }

    SECTION("when the world is not up yet")
    {
        engine.terrain.tesPresent = false;

        SECTION("should find nowhere")
        {
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 3).Ok());
        }
    }
}

TEST_CASE("AmbushSpawnPoints refuses ground with no navmesh at all", "[AmbushSpawnPoints][engine]")
{
    // Everything else about this world is usable — it is the ground that has
    // nothing to stand on, which is the difference between attackers
    // converging on the player and attackers standing inside a rock.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.visibility.pickHitFraction = 0.0f;

    SECTION("when the cell carries no navmesh")
    {
        SECTION("should find nowhere")
        {
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 3).Ok());
        }
    }
}

TEST_CASE("AmbushSpawnPoints refuses a request it cannot answer", "[AmbushSpawnPoints][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, kWorldEdge, kGround);
    engine.visibility.pickHitFraction = 0.0f;

    SECTION("when there is no player")
    {
        SECTION("should find nowhere")
        {
            REQUIRE_FALSE(AmbushSpawnPoints::Find(nullptr, 2500, 3).Ok());
        }
    }

    SECTION("when no attackers were asked for")
    {
        SECTION("should find nowhere")
        {
            REQUIRE_FALSE(AmbushSpawnPoints::Find(player, 2500, 0).Ok());
        }
    }
}

TEST_CASE("AmbushSpawnPoints keeps inside the configured band", "[AmbushSpawnPoints][engine]")
{
    EngineMock engine;
    auto* player = PlayerFacingNorth(engine);
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = kGround;
    engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, kWorldEdge, kGround);
    engine.visibility.pickHitFraction = 0.0f;

    SECTION("when far less is asked for than the floor allows")
    {
        const ConfiguredSettings settings{"[Beats]\niAmbushMinSpawnDistanceUnits=3000\n"
                                          "iAmbushMaxSpawnDistanceUnits=5500\n"};
        const auto result = AmbushSpawnPoints::Find(player, 200, 1);

        SECTION("should push out to the floor")
        {
            // Nearer than the floor and the player watches the ambush appear.
            REQUIRE(result.Ok());
            REQUIRE(DistanceFromPlayer(result.spawnPoints.front()) >= 2900.0f);
        }
    }

    SECTION("when more is asked for than the ceiling allows")
    {
        const ConfiguredSettings settings{"[Beats]\niAmbushMinSpawnDistanceUnits=2500\n"
                                          "iAmbushMaxSpawnDistanceUnits=2600\n"};
        const auto result = AmbushSpawnPoints::Find(player, 9000, 1);

        SECTION("should pull in to the ceiling")
        {
            // Farther than the ceiling and the approach takes so long the beat
            // has moved on before anyone arrives.
            REQUIRE(result.Ok());
            REQUIRE(DistanceFromPlayer(result.spawnPoints.front()) <= 2700.0f);
        }
    }
}
