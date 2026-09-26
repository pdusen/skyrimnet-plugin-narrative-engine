#include <StuckRecovery.h>

#include <EngineMock.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

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
