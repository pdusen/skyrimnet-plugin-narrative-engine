#include <StuckRecovery.h>

#include <FineRoads.h>

#include <EngineMock.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Tests for the escort that unsticks actors a beat placed in the world.
//
// The module exists because a position can pass every pre-spawn gate and still
// leave an actor unable to travel: the search validates a POINT, while the
// engine walks a COLLISION CAPSULE. Its escalation is four steps deep, and each
// one is a decision that is invisible in game except as an ambush that never
// arrives or an NPC teleporting on top of the player.
//
// Two of the rules are load-bearing and easy to lose. An actor IN COMBAT is
// never touched at any range — combat AI circles, holds position and searches,
// all of which read as stalled, and a warp on top of that destroys what the AI
// was doing so the next check warps it again. And the fallback cursor is never
// rewound when an actor starts moving: a position that already failed is not
// worth retrying.
//
// The world is a flat plane the harness fabricates. What this code has to get
// right is which points it asks about and what it does with a refusal, so a
// synthetic world answers those exactly where real geometry would only make
// them approximate.

namespace
{
    namespace StuckRecovery = NarrativeEngine::StuckRecovery;
    namespace MainThread = NarrativeEngine::MainThread;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::EngineMock;
    using RE::NiPoint3;
    using StuckRecovery::Action;
    using StuckRecovery::Escort;
    using StuckRecovery::Options;

    constexpr std::uint32_t kActorFormID = 0x0001A6A0u;

    // Far enough out that the arrived-distance rule never fires by accident.
    const NiPoint3 kGoal{0.0f, 0.0f, 0.0f};
    const NiPoint3 kFarFromGoal{10000.0f, 0.0f, 0.0f};

    template <class Fn> void OnMainThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            MainThread::Run(pt, [&](const MainThread::Token& mt) { body(mt); });
        });
    }

    // Places the actor at a position and hands it back. GetPosition is inline
    // and reads the object's own location field, so moving an actor between
    // checks is a matter of writing it.
    RE::Actor* PlaceActor(EngineMock& engine, const NiPoint3& at)
    {
        auto* actor = engine.AddActor(kActorFormID);
        actor->data.location = at;
        return actor;
    }

    void MoveActorTo(RE::Actor* actor, const NiPoint3& to)
    {
        actor->data.location = to;
    }

    StuckRecovery::Outcome Check(Escort& escort, RE::Actor* actor, const NiPoint3& goal, const Options& opts)
    {
        StuckRecovery::Outcome outcome;
        OnMainThread([&](const MainThread::Token& mt) { outcome = escort.Update(mt, actor, goal, opts); });
        return outcome;
    }
} // namespace

TEST_CASE("StuckRecovery::ActionName", "[StuckRecovery][engine]")
{
    SECTION("when an action is named")
    {
        SECTION("should give each one its own stable name")
        {
            // These reach the log and the dashboard, so they are a wire format
            // as much as a debugging aid.
            REQUIRE(std::string{StuckRecovery::ActionName(Action::None)} == "none");
            REQUIRE(std::string{StuckRecovery::ActionName(Action::Moving)} == "moving");
            REQUIRE(std::string{StuckRecovery::ActionName(Action::WarpedToFallback)} == "warped_to_fallback");
            REQUIRE(std::string{StuckRecovery::ActionName(Action::WarpedCloser)} == "warped_closer");
            REQUIRE(std::string{StuckRecovery::ActionName(Action::Stranded)} == "stranded");
        }
    }
}

TEST_CASE("StuckRecovery::Escort::DueForCheck", "[StuckRecovery][engine]")
{
    // Pure arithmetic over the caller's tick accumulator; the module never
    // reads a clock of its own.
    Escort escort{"test"};
    Options opts;
    opts.checkIntervalSeconds = 4.0;

    SECTION("when less than an interval has passed")
    {
        SECTION("should not be due")
        {
            REQUIRE_FALSE(escort.DueForCheck(1.0, opts));
            REQUIRE_FALSE(escort.DueForCheck(2.0, opts));
        }
    }

    SECTION("when the interval is reached")
    {
        SECTION("should be due")
        {
            REQUIRE_FALSE(escort.DueForCheck(3.9, opts));
            REQUIRE(escort.DueForCheck(0.2, opts));
        }

        SECTION("should not be due again immediately")
        {
            // The accumulator is consumed, not merely compared, so a caller
            // ticking every frame does not check on every one of them.
            REQUIRE(escort.DueForCheck(4.0, opts));
            REQUIRE_FALSE(escort.DueForCheck(0.1, opts));
        }
    }
}

TEST_CASE("StuckRecovery::Escort leaves a healthy actor alone", "[StuckRecovery][engine]")
{
    // Happy path, re-run per leaf: one actor placed far from its goal on flat
    // ground, with two vetted runner-up positions available.
    EngineMock engine;
    Escort escort{"test"};
    escort.Begin({NiPoint3{5000.0f, 0.0f, 0.0f}, NiPoint3{4000.0f, 0.0f, 0.0f}});
    auto* actor = PlaceActor(engine, kFarFromGoal);
    escort.Track(actor, kFarFromGoal);
    Options opts;

    SECTION("when the actor is travelling")
    {
        MoveActorTo(actor, NiPoint3{9000.0f, 0.0f, 0.0f});

        SECTION("should report it moving")
        {
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::Moving);
        }

        SECTION("should not move it")
        {
            Check(escort, actor, kGoal, opts);
            REQUIRE(engine.placement.moves.empty());
        }

        SECTION("should not rewind the fallback cursor")
        {
            // A position that already failed to work is not worth retrying if
            // this actor stalls again later, so a spell of movement must not
            // hand back the runner-ups already spent.
            Check(escort, actor, kGoal, opts);
            const auto stalled = Check(escort, actor, kGoal, opts);
            REQUIRE(stalled.action == Action::WarpedToFallback);
            REQUIRE(stalled.movedTo.x == 5000.0f);
        }
    }

    SECTION("when the actor is fighting")
    {
        engine.player.inCombat = true;

        SECTION("should leave it to the combat AI at any range")
        {
            // Load-bearing. Combat AI circles, holds position to shoot and
            // searches when it cannot perceive its target, all of which look
            // identical to being stuck — and a warp destroys whatever it was
            // doing, so the next check warps it again.
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::Moving);
            REQUIRE(engine.placement.moves.empty());
        }

        SECTION("should still advance the baseline for when combat ends")
        {
            // Otherwise the first check after a fight measures against where
            // the actor stood before it, and a long fight in one spot reads as
            // a long walk.
            Check(escort, actor, kGoal, opts);
            MoveActorTo(actor, NiPoint3{9990.0f, 0.0f, 0.0f});
            engine.player.inCombat = false;
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::WarpedToFallback);
        }
    }

    SECTION("when the actor has arrived")
    {
        MoveActorTo(actor, NiPoint3{1000.0f, 0.0f, 0.0f});

        SECTION("should leave it alone even though it is not moving")
        {
            // Stillness at this range is fighting or talking, not a trap, and
            // warping an actor mid-melee is worse than anything this solves.
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::Moving);
            REQUIRE(engine.placement.moves.empty());
        }
    }

    SECTION("when there is no actor")
    {
        SECTION("should do nothing")
        {
            StuckRecovery::Outcome outcome;
            OnMainThread([&](const MainThread::Token& mt) { outcome = escort.Update(mt, nullptr, kGoal, opts); });
            REQUIRE(outcome.action == Action::None);
        }
    }
}

TEST_CASE("StuckRecovery::Escort escalates through the runner-ups", "[StuckRecovery][engine]")
{
    // Happy path, re-run per leaf: a stalled actor with two runner-ups left.
    EngineMock engine;
    Escort escort{"test"};
    const NiPoint3 firstFallback{5000.0f, 0.0f, 0.0f};
    const NiPoint3 secondFallback{4000.0f, 0.0f, 0.0f};
    escort.Begin({firstFallback, secondFallback});
    auto* actor = PlaceActor(engine, kFarFromGoal);
    escort.Track(actor, kFarFromGoal);
    Options opts;

    SECTION("when the actor has not moved")
    {
        const auto outcome = Check(escort, actor, kGoal, opts);

        SECTION("should warp it to the first runner-up")
        {
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(outcome.movedTo.x == firstFallback.x);
        }

        SECTION("should actually move the actor")
        {
            REQUIRE(engine.placement.moves.size() == 1);
            REQUIRE(engine.placement.moves[0].x == firstFallback.x);
        }

        SECTION("should re-evaluate its package so it resumes travelling")
        {
            // Both the warp and the package re-evaluation are required. Without
            // the second the actor stands at its new position doing nothing,
            // which the next check reads as stuck again.
            REQUIRE(engine.placement.packageEvaluations >= 1);
        }

        SECTION("should measure the next interval from where it was put")
        {
            // The baseline is the destination, not the old position. Measuring
            // from the old one would count the warp itself as movement and
            // report the actor healthy on the very next check.
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::WarpedToFallback);
        }
    }

    SECTION("when it stalls again")
    {
        Check(escort, actor, kGoal, opts);

        SECTION("should move to the next runner-up rather than repeat the first")
        {
            const auto second = Check(escort, actor, kGoal, opts);
            REQUIRE(second.action == Action::WarpedToFallback);
            REQUIRE(second.movedTo.x == secondFallback.x);
        }
    }

    SECTION("when a new run begins")
    {
        Check(escort, actor, kGoal, opts);
        escort.Begin({firstFallback});
        auto* fresh = PlaceActor(engine, kFarFromGoal);
        escort.Track(fresh, kFarFromGoal);
        engine.placement.moves.clear();

        SECTION("should start from the first runner-up again")
        {
            // Begin replaces the list and clears tracking, so the next
            // encounter is not penalised for the last one's failures.
            const auto outcome = Check(escort, fresh, kGoal, opts);
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(outcome.movedTo.x == firstFallback.x);
        }
    }

    SECTION("when an actor is forgotten")
    {
        Check(escort, actor, kGoal, opts);

        SECTION("should treat it as new if it comes back")
        {
            // Called when an actor dies or is removed. What must NOT reset is
            // the fallback cursor, which belongs to the run rather than to any
            // one actor.
            escort.Forget(kActorFormID);
            escort.Track(actor, actor->GetPosition());
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(outcome.movedTo.x == secondFallback.x);
        }
    }
}

TEST_CASE("StuckRecovery::Escort strands an actor it cannot help", "[StuckRecovery][engine]")
{
    // No runner-ups at all, so the first stall goes straight to the close-in
    // step and, at this range, to giving up.
    EngineMock engine;
    Escort escort{"test"};
    escort.Begin({});
    Options opts;

    SECTION("when the actor is already inside the minimum goal distance")
    {
        // Between the two thresholds: far enough that it has not arrived, near
        // enough that there is no room left to close in.
        auto* actor = PlaceActor(engine, NiPoint3{1200.0f, 0.0f, 0.0f});
        escort.Track(actor, NiPoint3{1200.0f, 0.0f, 0.0f});
        opts.arrivedDistanceUnits = 1000.0f;
        opts.minGoalDistanceUnits = 1500.0f;
        const auto outcome = Check(escort, actor, kGoal, opts);

        SECTION("should give up rather than warp it onto the player")
        {
            // Below the minimum there is nowhere left to put it that is not on
            // top of whoever it was walking towards.
            REQUIRE(outcome.action == Action::Stranded);
            REQUIRE(engine.placement.moves.empty());
        }

        SECTION("should stay stranded even once the goal moves away again")
        {
            // Sticky on purpose, and this is the case that shows it: the goal
            // is the player, who walks off, so the distance that stranded the
            // actor stops holding. Re-deciding it from scratch would send an
            // actor that has already exhausted every option back through the
            // close-in ladder every four seconds for the rest of the encounter.
            REQUIRE(outcome.action == Action::Stranded);
            const NiPoint3 distantGoal{-8000.0f, 0.0f, 0.0f};
            REQUIRE(Check(escort, actor, distantGoal, opts).action == Action::Stranded);
        }
    }

    SECTION("when the ground under the close-in probe is unusable")
    {
        // Far enough out to warrant a close-in step, but the world refuses
        // every position: no land height resolves anywhere.
        auto* actor = PlaceActor(engine, kFarFromGoal);
        escort.Track(actor, kFarFromGoal);
        engine.terrain.landHeightResolves = false;

        SECTION("should leave the actor where it is and try again later")
        {
            // Not fatal. The next interval probes further along the same line,
            // which is more likely to clear whatever it was caught on than
            // giving up here would be.
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action == Action::None);
            REQUIRE(engine.placement.moves.empty());
        }

        SECTION("should give up once the probe has walked the whole line")
        {
            // The ladder is finite. An actor that never moves hands back the
            // same position every interval, so unless the reach grows the
            // probe asks about one spot for the rest of the encounter -- which
            // is what a visit did for sixty of its ninety seconds. Ten
            // thousand units at six hundred a step is sixteen probes; by then
            // the reach has run into the minimum goal distance and there is no
            // more line left to walk.
            Action last = Action::None;
            for (int i = 0; i < 20 && last != Action::Stranded; ++i) {
                last = Check(escort, actor, kGoal, opts).action;
            }
            REQUIRE(last == Action::Stranded);
            REQUIRE(engine.placement.moves.empty());
        }
    }

    SECTION("when only the far end of the line is standable")
    {
        // Navmesh near the goal and nothing between it and the actor -- the
        // shape of a visitor stopped on the wrong bank of a river. The early
        // probes land in the water; the reach has to carry a later one across.
        //
        // 600 a step off 6000 walks the probe to 5400, then 4800, then 4200,
        // and only that last one is inside the patch. Were the reach fixed it
        // would ask about 5400 every time and never move the actor at all.
        engine.world.cellIsInterior = false;
        engine.terrain.landHeight = 0.0f;
        engine.AddNavmeshPatch(engine.GroundCell(), 0x00A52001u, -1000.0f, -1000.0f, 4300.0f, 1000.0f, 0.0f);

        const NiPoint3 stuckAt{6000.0f, 0.0f, 0.0f};
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should reach further with each refusal until one lands")
        {
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::None);
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::None);

            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action == Action::WarpedCloser);
            REQUIRE(outcome.movedTo.x < stuckAt.x);
        }

        SECTION("should start the ladder over once a probe lands")
        {
            // The multiplier counts refusals SINCE the last placement, not
            // refusals ever. A warp that worked says the actor is somewhere
            // new, and the next stall there deserves a near step rather than
            // one that leaps three times as far because of ground it is no
            // longer standing on.
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::None);
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::None);

            const auto landed = Check(escort, actor, kGoal, opts);
            REQUIRE(landed.action == Action::WarpedCloser);
            MoveActorTo(actor, landed.movedTo);

            // One step of 600 from roughly 4200 out; carrying the two earlier
            // refusals over would take 1800 of it in one go.
            const auto next = Check(escort, actor, kGoal, opts);
            REQUIRE(next.action == Action::WarpedCloser);
            const float stepTaken = landed.movedTo.GetDistance(kGoal) - next.movedTo.GetDistance(kGoal);
            REQUIRE(stepTaken < 1.5f * opts.closeInStepUnits);
        }
    }
}

TEST_CASE("StuckRecovery::IsUnderwater", "[StuckRecovery][engine]")
{
    // Shared with the spawn searches, which ask the same question of their
    // candidates: a position under the local water surface is not somewhere an
    // actor can walk in from.
    EngineMock engine;

    SECTION("when the cell has water above the ground")
    {
        engine.terrain.hasWater = true;
        engine.terrain.waterHeight = 100.0f;

        SECTION("should say a point below the surface is underwater")
        {
            REQUIRE(StuckRecovery::IsUnderwater(nullptr, NiPoint3{0.0f, 0.0f, 50.0f}, 0.0f));
        }

        SECTION("should say a point above the surface is not")
        {
            REQUIRE_FALSE(StuckRecovery::IsUnderwater(nullptr, NiPoint3{0.0f, 0.0f, 200.0f}, 150.0f));
        }
    }

    SECTION("when the cell has no water")
    {
        engine.terrain.hasWater = false;

        SECTION("should say nothing is underwater")
        {
            REQUIRE_FALSE(StuckRecovery::IsUnderwater(nullptr, NiPoint3{0.0f, 0.0f, -500.0f}, -600.0f));
        }
    }
}

TEST_CASE("StuckRecovery::GroundPoint", "[StuckRecovery][engine]")
{
    EngineMock engine;

    SECTION("when the ground resolves")
    {
        engine.terrain.landHeightResolves = true;
        engine.terrain.landHeight = 250.0f;

        SECTION("should place the point just above the surface")
        {
            // Lifted rather than exactly on the ground: an actor placed
            // embedded is shoved out sideways, which is how it ends up back in
            // the geometry it was pulled from.
            NiPoint3 out{};
            float groundZ = 0.0f;
            REQUIRE(StuckRecovery::GroundPoint(NiPoint3{10.0f, 20.0f, 900.0f}, out, groundZ));
            REQUIRE(groundZ == 250.0f);
            REQUIRE(out.z > 250.0f);
            REQUIRE(out.z == 250.0f + StuckRecovery::kGroundClearanceUnits);
        }

        SECTION("should keep the horizontal position")
        {
            NiPoint3 out{};
            float groundZ = 0.0f;
            REQUIRE(StuckRecovery::GroundPoint(NiPoint3{10.0f, 20.0f, 900.0f}, out, groundZ));
            REQUIRE(out.x == 10.0f);
            REQUIRE(out.y == 20.0f);
        }
    }

    SECTION("when there is no ground")
    {
        engine.terrain.landHeightResolves = false;

        SECTION("should refuse rather than invent one")
        {
            // The void, or an unloaded cell. A fabricated height here would
            // drop an actor through the world.
            NiPoint3 out{};
            float groundZ = 0.0f;
            REQUIRE_FALSE(StuckRecovery::GroundPoint(NiPoint3{10.0f, 20.0f, 900.0f}, out, groundZ));
        }
    }
}

TEST_CASE("StuckRecovery::IsStandable on a street with no terrain under it", "[StuckRecovery][engine]")
{
    // A walled city. WhiterunWorld has a landscape record in 7 of its 113
    // cells and WindhelmWorld in 2 of 57: the ground somebody walks on in
    // there is authored static geometry, and the only thing that says it is
    // walkable is the navmesh over it.
    //
    // Measured before this worked: 45 of 45 arc samples inside Whiterun
    // rejected off-navmesh, so the city-approach tier never once chose a
    // point, and the escort's close-in probe found nowhere standable three
    // steps running on open street.
    constexpr std::uint32_t kStreetMesh = 0x00E10001u;
    constexpr float kStreetZ = -3240.0f;

    EngineMock engine;
    engine.AddNavmeshPatch(engine.GroundCell(), kStreetMesh, -4000.0f, -4000.0f, 4000.0f, 4000.0f, kStreetZ);

    SECTION("when the cell carries no landscape at all")
    {
        engine.terrain.landHeightResolves = false;

        SECTION("should stand them on the navmesh")
        {
            NiPoint3 out{};
            REQUIRE(StuckRecovery::IsStandable(NiPoint3{500.0f, 500.0f, kStreetZ}, out));
            REQUIRE(out.z == kStreetZ + StuckRecovery::kGroundClearanceUnits);
            REQUIRE(out.x == 500.0f);
            REQUIRE(out.y == 500.0f);
        }

        SECTION("should still refuse where there is no navmesh either")
        {
            // Off the edge of the patch. Nothing to read from either source,
            // which is the honest no.
            NiPoint3 out{};
            REQUIRE_FALSE(StuckRecovery::IsStandable(NiPoint3{9000.0f, 9000.0f, kStreetZ}, out));
        }
    }

    SECTION("when the landscape is there but far below the street")
    {
        // The other way this fails in a city, and the log cannot tell it from
        // the first: Whiterun sits on a plateau of static geometry, so the
        // terrain surface can be hundreds of units under the paving. The
        // terrain answer is then real and useless — it grounds the probe below
        // the navmesh, which reads as off-navmesh.
        engine.terrain.landHeightResolves = true;
        engine.terrain.landHeight = kStreetZ - 400.0f;

        SECTION("should take the street rather than the cellar floor")
        {
            NiPoint3 out{};
            REQUIRE(StuckRecovery::IsStandable(NiPoint3{500.0f, 500.0f, kStreetZ}, out));
            REQUIRE(out.z == kStreetZ + StuckRecovery::kGroundClearanceUnits);
        }
    }

    SECTION("when the navmesh is further off than the window allows")
    {
        // The reason the fallback is bounded. A path at the foot of a cliff is
        // navmesh under the XY of somebody standing on top of it, and warping
        // them down to it is not a correction.
        engine.terrain.landHeightResolves = false;

        SECTION("should refuse rather than drop them to it")
        {
            NiPoint3 out{};
            REQUIRE_FALSE(StuckRecovery::IsStandable(NiPoint3{500.0f, 500.0f, kStreetZ + 2000.0f}, out));
        }
    }
}

TEST_CASE("StuckRecovery::IsOnNavmesh", "[StuckRecovery][engine]")
{
    EngineMock engine;

    SECTION("when the world has no cell at the position")
    {
        engine.terrain.cellPresent = false;

        SECTION("should say no rather than assume")
        {
            // An unloaded cell is the common case at the edge of a spawn
            // search, and treating it as walkable would place actors in cells
            // the pathing system knows nothing about.
            REQUIRE_FALSE(StuckRecovery::IsOnNavmesh(NiPoint3{0.0f, 0.0f, 0.0f}));
        }
    }

    SECTION("when the world is not up yet")
    {
        engine.terrain.tesPresent = false;

        SECTION("should say no rather than crash")
        {
            REQUIRE_FALSE(StuckRecovery::IsOnNavmesh(NiPoint3{0.0f, 0.0f, 0.0f}));
        }
    }
}

TEST_CASE("StuckRecovery::Escort stops spending fallbacks on the same dead end", "[StuckRecovery][engine]")
{
    // What this is for, from the field: a visitor was placed, walked in, and
    // halted. The escort warped her to the next vetted position, she ran back
    // in, and halted within ten units of the same spot. Six times, consuming
    // seventy-two seconds of a ninety-second approach budget — and the
    // close-in step, the one move that could have got past the obstacle, never
    // got a turn.
    //
    // The tell was there from the second stall: an actor that comes to rest in
    // the same place from two different placements is saying the obstacle is
    // between that place and the goal, not under its feet.
    //
    // Note the shape of a cycle, which the log pins down. A warp resets the
    // baseline to the destination, so the check after it sees a large
    // displacement and reports the actor healthy while it walks back. Only the
    // check after THAT finds it stationary again. Every section here plays
    // that out rather than warping and re-checking, because a test that skips
    // the healthy check is testing a sequence the game never produces.
    EngineMock engine;
    // Ground and navmesh everywhere, so the close-in step has somewhere to go
    // and a refusal is the escort's decision rather than the world's.
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = 0.0f;
    engine.AddNavmeshPatch(engine.GroundCell(), 0x00A50001u, -40000.0f, -40000.0f, 40000.0f, 40000.0f, 0.0f);

    Escort escort{"test"};
    const NiPoint3 deadEnd{7000.0f, 0.0f, 0.0f};
    escort.Begin({NiPoint3{9000.0f, 0.0f, 0.0f},
                  NiPoint3{9500.0f, 500.0f, 0.0f},
                  NiPoint3{9500.0f, -500.0f, 0.0f},
                  NiPoint3{10000.0f, 0.0f, 0.0f}});
    auto* actor = PlaceActor(engine, deadEnd);
    escort.Track(actor, deadEnd);
    Options opts;

    // One full cycle: the actor is stalled, gets moved, walks to `walkedTo`,
    // and comes to rest there. Returns what the escort decides about that
    // resting place.
    const auto cycleTo = [&](const NiPoint3& walkedTo) {
        MoveActorTo(actor, walkedTo);
        REQUIRE(Check(escort, actor, kGoal, opts).action == Action::Moving);
        return Check(escort, actor, kGoal, opts);
    };

    SECTION("when it walks back to the same spot after a fallback")
    {
        // First stall: nothing known yet, so the list gets its turn.
        REQUIRE(Check(escort, actor, kGoal, opts).action == Action::WarpedToFallback);

        SECTION("should stop handing out fallbacks and close in instead")
        {
            const auto outcome = cycleTo(NiPoint3{deadEnd.x + 9.0f, deadEnd.y - 4.0f, 0.0f});
            REQUIRE(outcome.action == Action::WarpedCloser);
        }

        SECTION("should keep closing in rather than going back to the list")
        {
            REQUIRE(cycleTo(NiPoint3{deadEnd.x + 9.0f, deadEnd.y - 4.0f, 0.0f}).action == Action::WarpedCloser);
            REQUIRE(cycleTo(NiPoint3{deadEnd.x + 2.0f, deadEnd.y, 0.0f}).action == Action::WarpedCloser);
        }
    }

    SECTION("when it stalls somewhere genuinely different each time")
    {
        // The whole point of the vetted list is that a different placement
        // often does work, so noticing convergence must not cost that.
        REQUIRE(Check(escort, actor, kGoal, opts).action == Action::WarpedToFallback);

        SECTION("should carry on down the list")
        {
            const auto outcome = cycleTo(NiPoint3{6000.0f, 3000.0f, 0.0f});
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(outcome.movedTo.y == 500.0f);
        }
    }

    SECTION("when a fallback leaves it much further from the goal than it started")
    {
        // The second question, which catches the case where it stalls
        // somewhere new every time but each one is worse than the last.
        REQUIRE(Check(escort, actor, kGoal, opts).action == Action::WarpedToFallback);

        SECTION("should stop trusting the list")
        {
            const auto outcome = cycleTo(NiPoint3{12000.0f, 0.0f, 0.0f});
            REQUIRE(outcome.action == Action::WarpedCloser);
        }
    }

    SECTION("when a fallback leaves it only slightly further out")
    {
        // Runner-ups are ordered by how good a place they are to walk from,
        // not by distance, so the next one being a little further out is
        // ordinary and must not retire the list. Well clear of the first
        // stall, so this is the slack being tested and not convergence.
        REQUIRE(Check(escort, actor, kGoal, opts).action == Action::WarpedToFallback);

        SECTION("should carry on down the list")
        {
            // 7159 units out against the 7000 it started from.
            REQUIRE(cycleTo(NiPoint3{7000.0f, 1500.0f, 0.0f}).action == Action::WarpedToFallback);
        }
    }
}

TEST_CASE("StuckRecovery::Escort hops a stalled actor along the road", "[StuckRecovery][engine]")
{
    // The ladder's road half. An actor that stalls INSIDE the loaded grid is
    // caught on local geometry — a fence corner, a doorway, a boulder — and
    // the fix is a short hop onto validated road nearby, not a long warp back
    // out along the route that discards the walk it has already done.
    //
    // The goal is at the origin and the actor is 4,000 units east of it, so
    // "toward the goal" is west and "backwards" is east. The road below is
    // laid out so that each eligibility rule has a node that only it refuses.
    EngineMock engine;
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = 0.0f;
    engine.AddNavmeshPatch(engine.GroundCell(), 0x00A60001u, -40000.0f, -40000.0f, 40000.0f, 40000.0f, 0.0f);

    const NiPoint3 stuckAt{4000.0f, 0.0f, 0.0f};

    // Nodes, with what each one is for:
    //   (4100,0)   100u away — inside the minimum hop, so probably caught on
    //              the same obstacle.
    //   (4000,600) 600u away, perpendicular — the lateral hop the bound is
    //              supposed to allow: it costs only 44u of distance to goal.
    //   (4600,0)   600u away, straight backwards — costs the full 600u.
    //   (3400,0)   600u away, straight toward the goal — ideal, and nearer
    //              than the lateral one is in walked terms? No: same 600u, so
    //              the tiebreak is which comes first by hop length.
    const auto ladderFor = [&](std::vector<NiPoint3> nodes) {
        Escort::Ladder ladder;
        ladder.minHopUnits = 300.0f;
        ladder.maxRetreatUnits = 150.0f;
        for (const auto& node : nodes) {
            NarrativeEngine::FineRoads::Node fine;
            fine.x = node.x;
            fine.y = node.y;
            fine.z = node.z;
            ladder.fine.nodes.push_back(fine);
            ladder.fine.adjacency.emplace_back();
        }
        return ladder;
    };

    Options opts;

    SECTION("when a node is nearer than the minimum hop")
    {
        Escort escort{"test"};
        escort.BeginLadder(ladderFor({NiPoint3{4100.0f, 0.0f, 0.0f}}));
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should refuse it and fall through")
        {
            // The road graph is dense along a road, and the nodes closest to a
            // stalled actor are the ones most likely caught on the same thing.
            // With no chain fallbacks either, that leaves the close-in step.
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action != Action::WarpedToFallback);
        }
    }

    SECTION("when a node is far enough and sideways")
    {
        Escort escort{"test"};
        escort.BeginLadder(ladderFor({NiPoint3{4000.0f, 600.0f, 0.0f}}));
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should hop onto it")
        {
            // A 600-unit hop perpendicular to the goal from 4,000 units out
            // costs about 600*600/(2*4000) = 45 units of distance to the goal,
            // well inside the 150-unit bound. Sideways movement is almost
            // free, which is the whole reason a small bound is permissive.
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(std::fabs(outcome.movedTo.y - 600.0f) < 1.0f);
        }
    }

    SECTION("when the only node is straight backwards")
    {
        Escort escort{"test"};
        escort.BeginLadder(ladderFor({NiPoint3{4600.0f, 0.0f, 0.0f}}));
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should refuse it rather than undo the approach")
        {
            // 600 units directly away from the goal costs the full 600, four
            // times the bound. Hops are undirected, so without this a hop is
            // as free to move the actor backwards as forwards -- and a
            // backwards hop is not recovery.
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action != Action::WarpedToFallback);
        }
    }

    SECTION("when two nodes sit either side of one obstacle")
    {
        Escort escort{"test"};
        escort.BeginLadder(ladderFor({NiPoint3{4000.0f, 600.0f, 0.0f}, NiPoint3{4000.0f, -600.0f, 0.0f}}));
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should spend each one once and then stop")
        {
            // Without consumption the two trade the actor back and forth, each
            // hop looking like progress and none of it being any. A node is
            // spent whether or not the hop worked, because a node that did not
            // dislodge the actor is evidence against itself.
            // Each cycle stalls somewhere NEW, more than
            // kStallConvergenceUnits from the places before it. Coming to rest
            // twice in one spot is a different finding — the obstacle is
            // between there and the goal — and the escort already retires the
            // whole ladder for it, which would mask what this case is about.
            const auto first = Check(escort, actor, kGoal, opts);
            REQUIRE(first.action == Action::WarpedToFallback);
            MoveActorTo(actor, NiPoint3{4000.0f, 300.0f, 0.0f});
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::Moving);
            const auto second = Check(escort, actor, kGoal, opts);
            REQUIRE(second.action == Action::WarpedToFallback);
            REQUIRE(std::fabs(second.movedTo.y - first.movedTo.y) > 1.0f);

            // Both spent now. The third stall has no road left.
            MoveActorTo(actor, NiPoint3{4000.0f, -300.0f, 0.0f});
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::Moving);
            const auto third = Check(escort, actor, kGoal, opts);
            REQUIRE(third.action != Action::WarpedToFallback);
        }
    }

    SECTION("when the road is spent but chain points remain")
    {
        Escort escort{"test"};
        auto ladder = ladderFor({NiPoint3{4000.0f, 600.0f, 0.0f}});
        ladder.chainOutward = {NiPoint3{6000.0f, 0.0f, 0.0f}, NiPoint3{8000.0f, 0.0f, 0.0f}};
        escort.BeginLadder(std::move(ladder));
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should take the road first and the chain after")
        {
            // The road hop comes first because it discards less: the actor
            // keeps the distance it has already walked. Only once there is no
            // road left does the escort start walking it back out along the
            // route.
            const auto first = Check(escort, actor, kGoal, opts);
            REQUIRE(first.action == Action::WarpedToFallback);
            REQUIRE(std::fabs(first.movedTo.y - 600.0f) < 1.0f);

            // A new stall spot, for the reason given in the case above.
            MoveActorTo(actor, NiPoint3{4000.0f, 300.0f, 0.0f});
            REQUIRE(Check(escort, actor, kGoal, opts).action == Action::Moving);
            const auto second = Check(escort, actor, kGoal, opts);
            REQUIRE(second.action == Action::WarpedToFallback);
            REQUIRE(std::fabs(second.movedTo.x - 6000.0f) < 1.0f);
        }
    }

    SECTION("when there is no road graph at all")
    {
        Escort escort{"test"};
        Escort::Ladder ladder;
        ladder.chainOutward = {NiPoint3{6000.0f, 0.0f, 0.0f}};
        escort.BeginLadder(std::move(ladder));
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should go straight to the chain")
        {
            // Outside the loaded grid there is no graph to read, which is the
            // ordinary case for a visitor who has not reached the player's
            // cells yet. An empty graph sends every escalation to the chain.
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(std::fabs(outcome.movedTo.x - 6000.0f) < 1.0f);
        }
    }

    SECTION("when the actor is off navmesh")
    {
        Escort escort{"test"};
        auto ladder = ladderFor({NiPoint3{4000.0f, 600.0f, 0.0f}});
        ladder.chainOutward = {NiPoint3{6000.0f, 0.0f, 0.0f}};
        escort.BeginLadder(std::move(ladder));
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);
        // No navmesh anywhere, which is what being outside the loaded grid
        // looks like to every query this module has.
        engine.terrain.cellPresent = false;

        SECTION("should not try to hop it along a road it cannot be on")
        {
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(std::fabs(outcome.movedTo.x - 6000.0f) < 1.0f);
        }
    }

    SECTION("when a plain Begin is used instead")
    {
        Escort escort{"test"};
        escort.Begin(std::vector<NiPoint3>{NiPoint3{6000.0f, 0.0f, 0.0f}});
        auto* actor = PlaceActor(engine, stuckAt);
        escort.Track(actor, stuckAt);

        SECTION("should behave exactly as it did before the ladder existed")
        {
            // AmbushBeat's call, untouched by Phase 16. The road half has to
            // be inert for it rather than merely unused, which is why the two
            // numbers live on the Ladder and not in Options.
            const auto outcome = Check(escort, actor, kGoal, opts);
            REQUIRE(outcome.action == Action::WarpedToFallback);
            REQUIRE(std::fabs(outcome.movedTo.x - 6000.0f) < 1.0f);
        }
    }
}

TEST_CASE("StuckRecovery::HasNavmeshCorridor", "[StuckRecovery][engine]")
{
    // Containment cannot answer connectivity: an unreachable ledge across a
    // ravine is perfectly good navmesh, and every gate the arrival search runs
    // is about standing on a point rather than reaching it. This samples the
    // straight line instead, which is cheap and catches the gap that stranded
    // a visitor 550 units below the player.
    EngineMock engine;
    engine.world.cellIsInterior = false;
    engine.terrain.landHeight = 0.0f;

    const NiPoint3 here{0.0f, 0.0f, 0.0f};
    const NiPoint3 across{4000.0f, 0.0f, 0.0f};

    SECTION("when navmesh runs the whole way")
    {
        engine.AddNavmeshPatch(engine.GroundCell(), 0x00A51001u, -1000.0f, -1000.0f, 5000.0f, 1000.0f, 0.0f);

        SECTION("should say the line is walkable")
        {
            REQUIRE(StuckRecovery::HasNavmeshCorridor(here, across));
        }
    }

    SECTION("when a wide gap sits in between")
    {
        // Two islands with 2000 units of nothing between them — a ravine, or
        // the foot of a cliff the far side of which is where the player is.
        engine.AddNavmeshPatch(engine.GroundCell(), 0x00A51002u, -1000.0f, -1000.0f, 1000.0f, 1000.0f, 0.0f);
        engine.AddNavmeshPatch(engine.GroundCell(), 0x00A51003u, 3000.0f, -1000.0f, 5000.0f, 1000.0f, 0.0f);

        SECTION("should refuse the line")
        {
            REQUIRE_FALSE(StuckRecovery::HasNavmeshCorridor(here, across));
        }
    }

    SECTION("when the two ends are close together")
    {
        // Nothing between them worth sampling, and refusing here would reject
        // every candidate at the near edge of the band.
        SECTION("should not invent an obstacle")
        {
            REQUIRE(StuckRecovery::HasNavmeshCorridor(here, NiPoint3{100.0f, 0.0f, 0.0f}));
        }
    }

    SECTION("when no ground resolves anywhere")
    {
        engine.terrain.landHeightResolves = false;

        SECTION("should refuse rather than assume a corridor")
        {
            REQUIRE_FALSE(StuckRecovery::HasNavmeshCorridor(here, across));
        }
    }
}
