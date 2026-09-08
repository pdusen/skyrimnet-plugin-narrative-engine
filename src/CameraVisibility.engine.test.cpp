#include <CameraVisibility.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

// Tests for the "could the player see this if they turned round" check.
//
// The module exists to work around a specific engine behaviour, and that is the
// thing most at risk of being lost. `Actor::HasLineOfSight` gates on the
// player's field-of-view cone as well as on geometry, so it says no whenever
// the camera happens to be pointed away — in open air, with nothing in between.
// Callers here want the facing-INDEPENDENT question, so the engine call is
// trusted as a positive short-circuit only. Inverting it would be an easy
// simplification to make and would silently reintroduce the whole bug: an NPC
// teleporting home in plain sight, every time the player looked elsewhere.
//
// Everything else is a gate ordered cheapest-first, and each one has a reason
// to fail open or closed that is not obvious from the code.
//
// The engine side is stood in for at the raycast, not as geometry: the fan asks
// one question many times — did this ray reach its endpoint — so a single hit
// fraction says everything a world of obstacles would, and exactly.

namespace
{
    namespace CameraVisibility = NarrativeEngine::CameraVisibility;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using RE::NiPoint3;

    constexpr std::uint32_t kActorFormID = 0x0001A6A0u;

    // The module's own floor is 300 units; these sit either side of it with
    // room to spare.
    constexpr float kWellBeyondFloor = 2000.0f;
    constexpr float kInsideFloor = 100.0f;

    // A hit fraction below the module's 0.95 threshold: something solid stopped
    // the ray well short of the target.
    constexpr float kBlocked = 0.4f;
    constexpr float kClear = 1.0f;

    RE::Actor* PlaceActorAt(EngineMock& engine, float x)
    {
        auto* actor = engine.AddActor(kActorFormID);
        actor->data.location = NiPoint3{x, 0.0f, 0.0f};
        return actor;
    }
} // namespace

TEST_CASE("CameraVisibility::IsAnyPartVisibleFromCamera refuses to guess", "[CameraVisibility][engine]")
{
    // Happy path, re-run per leaf: a camera at the origin, a target well beyond
    // the close-range floor, 3D loaded, and clear air between them.
    EngineMock engine;
    auto* target = PlaceActorAt(engine, kWellBeyondFloor);

    SECTION("when there is no target")
    {
        SECTION("should say not visible")
        {
            REQUIRE_FALSE(CameraVisibility::IsAnyPartVisibleFromCamera(nullptr));
        }
    }

    SECTION("when the target has no 3D")
    {
        engine.visibility.target3DPresent = false;

        SECTION("should say not visible without casting a single ray")
        {
            // The cheapest gate, and the exact state the caller is hoping for:
            // an actor with no 3D is not rendered anywhere, so moving it home
            // cannot be seen. Asserting on the ray count as well as the answer
            // is what keeps this from passing on a coincidence.
            REQUIRE_FALSE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
            REQUIRE(engine.visibility.pickCalls == 0);
        }
    }

    SECTION("when the camera cannot be resolved")
    {
        engine.visibility.cameraPresent = false;

        SECTION("should fail closed")
        {
            // Fails closed rather than open: the caller has a timeout as a
            // backstop, so a spurious "not visible" costs one tick, while a
            // spurious "visible" would stall the beat until that timeout.
            REQUIRE_FALSE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
        }
    }

    SECTION("when the camera has no root node")
    {
        engine.visibility.cameraRootPresent = false;

        SECTION("should fail closed")
        {
            REQUIRE_FALSE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
        }
    }
}

TEST_CASE("CameraVisibility trusts the engine one way only", "[CameraVisibility][engine]")
{
    // The whole reason this module exists. Both leaves put a solid wall in
    // front of the target, so the only thing that can produce a "visible" is
    // the engine short-circuit — and the only thing that can produce a
    // "not visible" is the fan being consulted rather than the engine's answer
    // being inverted.
    EngineMock engine;
    auto* target = PlaceActorAt(engine, kWellBeyondFloor);
    engine.visibility.pickHitFraction = kBlocked;

    SECTION("when the engine says it can see the target")
    {
        engine.visibility.engineLineOfSight = true;

        SECTION("should take that as a yes and stop")
        {
            // A true from the engine means the raycast AND the cone both
            // passed, so there is nothing left to check.
            REQUIRE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
            REQUIRE(engine.visibility.pickCalls == 0);
        }
    }

    SECTION("when the engine says it cannot")
    {
        engine.visibility.engineLineOfSight = false;

        SECTION("should not take that as a no")
        {
            // The false may only mean "outside the field of view", which is the
            // bug being worked around. The fan has to run.
            (void)CameraVisibility::IsAnyPartVisibleFromCamera(target);
            REQUIRE(engine.visibility.pickCalls > 0);
        }
    }
}

TEST_CASE("CameraVisibility::IsAnyPartVisibleFromCamera casts a fan", "[CameraVisibility][engine]")
{
    // Happy path, re-run per leaf: engine LOS says no, so every case reaches
    // the fan and its answer is the fan's own.
    EngineMock engine;
    auto* target = PlaceActorAt(engine, kWellBeyondFloor);
    engine.visibility.engineLineOfSight = false;

    SECTION("when nothing is in the way")
    {
        engine.visibility.pickHitFraction = kClear;

        SECTION("should say visible")
        {
            REQUIRE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
        }

        SECTION("should stop at the first ray that gets through")
        {
            // Only "any part visible" is being asked, so continuing to cast
            // after a success is work done for nothing on the main thread,
            // every tick, for every watched actor.
            (void)CameraVisibility::IsAnyPartVisibleFromCamera(target);
            REQUIRE(engine.visibility.pickCalls == 1);
        }
    }

    SECTION("when every ray is stopped short")
    {
        engine.visibility.pickHitFraction = kBlocked;

        SECTION("should say not visible")
        {
            REQUIRE_FALSE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
        }

        SECTION("should have sampled several points rather than one")
        {
            // A single centre ray would call a target behind a narrow pillar
            // invisible while its head and shoulders were in plain sight.
            (void)CameraVisibility::IsAnyPartVisibleFromCamera(target);
            REQUIRE(engine.visibility.pickCalls > 1);
        }
    }

    SECTION("when a ray stops just short of its endpoint")
    {
        // A ray aimed at a bone's world position lands on that bone's collision
        // shape a few units early, so a strict "reached exactly" test would
        // call every visible actor occluded.
        engine.visibility.pickHitFraction = 0.99f;

        SECTION("should count that as reaching")
        {
            REQUIRE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
        }
    }

    SECTION("when the target's bound has no size yet")
    {
        // Real on freshly-attached 3D. The four bbox extremes would collapse
        // onto the centre, so they are skipped and the fan falls back to the
        // target's own position rather than casting nothing.
        engine.visibility.targetBoundRadius = 0.0f;
        engine.visibility.pickHitFraction = kClear;

        SECTION("should still cast at least one ray")
        {
            REQUIRE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
            REQUIRE(engine.visibility.pickCalls >= 1);
        }
    }

    SECTION("when the target has skeleton nodes")
    {
        // An actor, rather than a crate. The named nodes are sampled in
        // addition to the bounding box, which is what puts rays on a head and
        // shoulders rather than only on a box's corners.
        engine.visibility.namedNodes["NPC Head [Head]"] = true;
        engine.visibility.pickHitFraction = kBlocked;

        SECTION("should sample them as well as the bounding box")
        {
            const int withoutNodes = [&] {
                engine.visibility.namedNodes.clear();
                engine.visibility.pickCalls = 0;
                (void)CameraVisibility::IsAnyPartVisibleFromCamera(target);
                return engine.visibility.pickCalls;
            }();
            engine.visibility.namedNodes["NPC Head [Head]"] = true;
            engine.visibility.pickCalls = 0;
            (void)CameraVisibility::IsAnyPartVisibleFromCamera(target);
            REQUIRE(engine.visibility.pickCalls > withoutNodes);
        }
    }

    SECTION("when debug logging is on")
    {
        const ConfiguredSettings settings{"[General]\nbDebugMode=1\n"};
        engine.visibility.pickHitFraction = kClear;

        SECTION("should still answer the same")
        {
            // The debug arm stops short-circuiting so the summary can report
            // how many of the samples got through. It must not change the
            // verdict, only how much work reaching it costs.
            REQUIRE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
        }
    }
}

TEST_CASE("CameraVisibility treats point-blank range as visible", "[CameraVisibility][engine]")
{
    EngineMock engine;

    SECTION("when the target is inside the close-range floor")
    {
        auto* target = PlaceActorAt(engine, kInsideFloor);
        engine.visibility.pickHitFraction = kBlocked;
        engine.visibility.engineLineOfSight = false;

        SECTION("should say visible without casting")
        {
            // Below the floor a from-camera ray can start inside the target's
            // own collision or inside cover, and both produce nonsense. The
            // safe default at this range is "the player can see them", because
            // teleporting someone standing next to the player is the worst
            // outcome available.
            REQUIRE(CameraVisibility::IsAnyPartVisibleFromCamera(target));
            REQUIRE(engine.visibility.pickCalls == 0);
        }
    }
}

TEST_CASE("CameraVisibility::IsPositionBehindCover", "[CameraVisibility][engine]")
{
    // The inverse question, asked of a candidate spawn point rather than an
    // actor: is every part of a body-sized silhouette here blocked from the
    // camera. Nine rays, and all nine have to be stopped.
    EngineMock engine;
    const NiPoint3 spot{kWellBeyondFloor, 0.0f, 0.0f};

    SECTION("when everything is blocked")
    {
        engine.visibility.pickHitFraction = kBlocked;

        SECTION("should say the spot is covered")
        {
            REQUIRE(CameraVisibility::IsPositionBehindCover(spot, 128.0f, 40.0f));
        }

        SECTION("should test a silhouette rather than a point")
        {
            // A single line would let a lamp post pass as cover for a whole
            // body, and an ambusher would appear out of thin air beside it.
            (void)CameraVisibility::IsPositionBehindCover(spot, 128.0f, 40.0f);
            REQUIRE(engine.visibility.pickCalls > 1);
        }
    }

    SECTION("when any line is clear")
    {
        engine.visibility.pickHitFraction = kClear;

        SECTION("should say the spot is not covered")
        {
            REQUIRE_FALSE(CameraVisibility::IsPositionBehindCover(spot, 128.0f, 40.0f));
        }
    }

    SECTION("when the spot is too close to raycast meaningfully")
    {
        engine.visibility.pickHitFraction = kBlocked;

        SECTION("should refuse to call it covered")
        {
            // Same floor as the visibility check, and the same reason: a very
            // short ray can start inside nearby geometry and report nonsense.
            // Nothing this close to the player counts as hidden.
            REQUIRE_FALSE(CameraVisibility::IsPositionBehindCover(NiPoint3{kInsideFloor, 0.0f, 0.0f}, 128.0f, 40.0f));
        }
    }

    SECTION("when the camera is directly overhead")
    {
        engine.visibility.pickHitFraction = kBlocked;
        engine.visibility.cameraX = 0.0f;
        engine.visibility.cameraY = 0.0f;
        engine.visibility.cameraZ = 5000.0f;

        SECTION("should refuse rather than divide by a zero silhouette")
        {
            // Straight down there is no horizontal axis to widen the test
            // along, so there is no silhouette to prove blocked.
            REQUIRE_FALSE(CameraVisibility::IsPositionBehindCover(NiPoint3{0.0f, 0.0f, 0.0f}, 128.0f, 40.0f));
        }
    }

    SECTION("when the camera cannot be resolved")
    {
        engine.visibility.cameraPresent = false;
        engine.visibility.pickHitFraction = kBlocked;

        SECTION("should refuse to call it covered")
        {
            // No camera, no way to prove cover. Failing open here is the safe
            // direction: an ambush that declines a spot costs nothing, while
            // one that spawns in the open is the failure players report.
            REQUIRE_FALSE(CameraVisibility::IsPositionBehindCover(spot, 128.0f, 40.0f));
        }
    }
}
