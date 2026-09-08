#include <Region.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

// Mocked-engine tests for hold and biome resolution.
//
// "Which hold is the player in" is the identity every travel event, every
// cross-boundary check and every biome-gated beat is keyed on, and it is
// derived rather than stored: the module walks the parentLoc chain until it
// finds an ancestor carrying the vanilla LocTypeHold keyword. Getting that
// wrong does not crash — it silently attributes a journey to the wrong hold, or
// reports no hold at all and makes the Director go quiet in a region.
//
// The module has two ways to reach the same answer, and they must agree. The
// fast path asks HoldGrid for a precomputed coordinate-to-hold FormID; the
// fallback walks the chain. HoldGrid is our own module and linking it would
// drag the grid builder into this executable, so the test defines
// HoldGrid::LookupPlayer itself. That is what makes both paths reachable at
// all — with the real one there would be no way to say "the grid has no answer
// here" from a test.
//
// Two caches are function-local statics, resolved once per process: the
// LocTypeHold keyword and the FormID-to-Climate table. Whichever case runs
// first therefore fixes both for the whole run, so every case registers the
// full set of holds up front and always at the same FormIDs. The
// keyword-missing branch is unreachable for the same reason and is called out
// below.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace Region = NarrativeEngine::Region;
    using Region::Climate;

    // What the stand-in grid answers with. Zero means "the grid has no entry
    // for where the player is standing", which is the normal case for every
    // interior cell.
    RE::FormID g_gridAnswer = 0;

    // Hold FormIDs. Fixed per editor ID for the life of the process, because
    // the climate table caches the mapping the first time it resolves.
    constexpr RE::FormID kWhiterunHold = 0x00016BE4u;
    constexpr RE::FormID kFalkreathHold = 0x0001680Fu;
    constexpr RE::FormID kEastmarchHold = 0x0001680Eu;
    constexpr RE::FormID kUnlistedHold = 0x00FF0001u;
    constexpr RE::FormID kChildLocation = 0x00018A56u;
    constexpr RE::FormID kGrandchildLocation = 0x00018A57u;

    // Registers every editor ID the two process-wide caches will ever look up,
    // at the FormIDs the cases expect. Called by every TEST_CASE, because
    // whichever one runs first is the one that actually builds the caches.
    void RegisterHolds(EngineMock& engine)
    {
        (void)engine.AddKeyword("LocTypeHold");
        (void)engine.AddLocation(kWhiterunHold, "Whiterun Hold", {"LocTypeHold"}, "WhiterunHoldLocation");
        (void)engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
        (void)engine.AddLocation(kEastmarchHold, "Eastmarch Hold", {"LocTypeHold"}, "EastmarchHoldLocation");
        // The remaining seven never appear in a case, but the climate table
        // resolves all ten on the first call and logs a warning for each one it
        // cannot find. Registering them keeps that noise out of the run.
        for (const char* edid : {"HjaalmarchHoldLocation",
                                 "WinterholdHoldLocation",
                                 "PaleHoldLocation",
                                 "ReachHoldLocation",
                                 "RiftHoldLocation",
                                 "HaafingarHoldLocation",
                                 "DLC2SolstheimLocation"}) {
            static RE::FormID next = 0x00FE0000u;
            (void)engine.AddLocation(++next, edid, {"LocTypeHold"}, edid);
        }
    }
} // namespace

// The grid fast path. Defining it here keeps HoldGrid out of this link closure
// and is the only way a case can say "the grid has nothing for this position".
namespace NarrativeEngine::HoldGrid
{
    RE::FormID LookupPlayer()
    {
        return g_gridAnswer;
    }
} // namespace NarrativeEngine::HoldGrid

TEST_CASE("Region::ForLocation walks the parent chain", "[Region][engine]")
{
    // Happy path, re-run per leaf: the grid is silent, so every case here
    // exercises the walk. The player's own location is not a hold.
    EngineMock engine;
    RegisterHolds(engine);
    g_gridAnswer = 0;

    SECTION("when the location is itself a hold")
    {
        auto* hold = engine.AddLocation(kWhiterunHold, "Whiterun Hold", {"LocTypeHold"}, "WhiterunHoldLocation");

        SECTION("should resolve without walking")
        {
            const auto r = Region::ForLocation(hold);
            REQUIRE(r.holdFormID == kWhiterunHold);
            REQUIRE(r.holdDisplayName == "Whiterun Hold");
        }
    }

    SECTION("when the hold is the immediate parent")
    {
        auto* hold = engine.AddLocation(kWhiterunHold, "Whiterun Hold", {"LocTypeHold"}, "WhiterunHoldLocation");
        auto* child = engine.AddLocation(kChildLocation, "Whiterun Stables", {});
        engine.SetLocationParent(child, hold);

        SECTION("should resolve the parent")
        {
            REQUIRE(Region::ForLocation(child).holdFormID == kWhiterunHold);
        }
    }

    SECTION("when the hold is further up the chain")
    {
        // The shape that matters. An interior in a building in a city in a
        // hold is four links, and vanilla's own quest conditions read it the
        // same way.
        auto* hold = engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
        auto* city = engine.AddLocation(kChildLocation, "Falkreath", {"LocTypeTown"});
        auto* inn = engine.AddLocation(kGrandchildLocation, "Dead Man's Drink", {});
        engine.SetLocationParent(city, hold);
        engine.SetLocationParent(inn, city);

        SECTION("should resolve the hold")
        {
            const auto r = Region::ForLocation(inn);
            REQUIRE(r.holdFormID == kFalkreathHold);
            REQUIRE(r.climate == Climate::Pine);
        }
    }

    SECTION("when nothing in the chain is a hold")
    {
        auto* orphan = engine.AddLocation(kChildLocation, "Some Test Cell", {"LocTypeDungeon"});

        SECTION("should report no hold")
        {
            // A real state, not an error: a few scripted interiors and the
            // pre-vanilla test cells genuinely belong to no hold. Callers read
            // this as "no signal" rather than as a failure.
            const auto r = Region::ForLocation(orphan);
            REQUIRE(r.holdFormID == 0);
            REQUIRE(r.holdDisplayName.empty());
            REQUIRE(r.climate == Climate::Unknown);
        }
    }

    SECTION("when the chain forms a cycle")
    {
        // Malformed mod data. The walk is depth-capped rather than
        // cycle-detecting, so this has to terminate rather than spin.
        auto* first = engine.AddLocation(kChildLocation, "First", {});
        auto* second = engine.AddLocation(kGrandchildLocation, "Second", {});
        engine.SetLocationParent(first, second);
        engine.SetLocationParent(second, first);

        SECTION("should terminate and report no hold")
        {
            REQUIRE(Region::ForLocation(first).holdFormID == 0);
        }
    }

    SECTION("when the location is null")
    {
        SECTION("should report no hold")
        {
            REQUIRE(Region::ForLocation(nullptr).holdFormID == 0);
        }
    }
}

TEST_CASE("Region resolves a hold's climate and name", "[Region][engine]")
{
    // Happy path, re-run per leaf: a hold that is in the climate table and has
    // a display name.
    EngineMock engine;
    RegisterHolds(engine);
    g_gridAnswer = 0;

    SECTION("when the hold is in the climate table")
    {
        SECTION("should report its climate")
        {
            auto* hold = engine.AddLocation(kEastmarchHold, "Eastmarch Hold", {"LocTypeHold"}, "EastmarchHoldLocation");
            REQUIRE(Region::ForLocation(hold).climate == Climate::Volcanic);
        }

        SECTION("should distinguish holds that share a climate")
        {
            // Winterhold and the Pale both map to Snow, so a table keyed on
            // climate rather than FormID would collapse them.
            auto* whiterun =
                engine.AddLocation(kWhiterunHold, "Whiterun Hold", {"LocTypeHold"}, "WhiterunHoldLocation");
            REQUIRE(Region::ForLocation(whiterun).climate == Climate::Tundra);
        }
    }

    SECTION("when the hold is not in the climate table")
    {
        auto* hold = engine.AddLocation(kUnlistedHold, "Some Modded Hold", {"LocTypeHold"}, "ModdedHoldLocation");

        SECTION("should still resolve the hold")
        {
            // Non-fatal by design. A hold added by a mod is not in our table,
            // but travel detection only needs the identity — losing the whole
            // resolution over a missing climate would silence the Director
            // across every modded region.
            const auto r = Region::ForLocation(hold);
            REQUIRE(r.holdFormID == kUnlistedHold);
            REQUIRE(r.holdDisplayName == "Some Modded Hold");
            REQUIRE(r.climate == Climate::Unknown);
        }
    }

    SECTION("when the hold has no display name")
    {
        auto* hold = engine.AddLocation(kUnlistedHold, "", {"LocTypeHold"}, "NamelessHoldLocation");

        SECTION("should fall back to the editor id")
        {
            REQUIRE(Region::ForLocation(hold).holdDisplayName == "NamelessHoldLocation");
        }
    }

    SECTION("when the hold has neither name nor editor id")
    {
        auto* hold = engine.AddLocation(kUnlistedHold, "", {"LocTypeHold"});

        SECTION("should leave the name empty but keep the identity")
        {
            const auto r = Region::ForLocation(hold);
            REQUIRE(r.holdDisplayName.empty());
            REQUIRE(r.holdFormID == kUnlistedHold);
        }
    }
}

TEST_CASE("Region::ForPlayer", "[Region][engine]")
{
    // Happy path, re-run per leaf: the grid answers with Whiterun Hold, which
    // is the case for every exterior cell the builder reached.
    EngineMock engine;
    RegisterHolds(engine);
    g_gridAnswer = kWhiterunHold;

    SECTION("when the grid has an answer")
    {
        SECTION("should use it")
        {
            REQUIRE(Region::ForPlayer().holdFormID == kWhiterunHold);
        }

        SECTION("should not consult the player's location chain")
        {
            // The point of the fast path. The player is standing somewhere the
            // walk would resolve to Falkreath, so a resolution naming Whiterun
            // can only have come from the grid.
            auto* falkreath =
                engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
            auto* here = engine.AddLocation(kChildLocation, "Somewhere", {});
            engine.SetLocationParent(here, falkreath);
            engine.world.playerLocationOverride = here;
            REQUIRE(Region::ForPlayer().holdFormID == kWhiterunHold);
        }
    }

    SECTION("when the grid names a form that is not a location")
    {
        // The grid stores raw FormIDs, so a stale or mismatched entry can point
        // at something that is not a BGSLocation any more. Falling through to
        // the walk is safer than trusting the cast — an actor read as a
        // location would hand back whatever bytes sit where parentLoc belongs.
        constexpr RE::FormID kActorFormID = 0x0001A6A0u;
        (void)engine.AddActor(kActorFormID);
        g_gridAnswer = kActorFormID;

        SECTION("should fall back to the parent walk")
        {
            auto* falkreath =
                engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
            engine.world.playerLocationOverride = falkreath;
            REQUIRE(Region::ForPlayer().holdFormID == kFalkreathHold);
        }
    }

    SECTION("when the grid names a form that does not resolve")
    {
        g_gridAnswer = 0xDEADBEEFu;

        SECTION("should fall back to the parent walk")
        {
            auto* falkreath =
                engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
            engine.world.playerLocationOverride = falkreath;
            REQUIRE(Region::ForPlayer().holdFormID == kFalkreathHold);
        }
    }

    SECTION("when the grid has no answer")
    {
        // Every interior cell, which the grid does not index at all.
        g_gridAnswer = 0;

        SECTION("should walk the player's location chain")
        {
            auto* falkreath =
                engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
            auto* here = engine.AddLocation(kChildLocation, "Somewhere", {});
            engine.SetLocationParent(here, falkreath);
            engine.world.playerLocationOverride = here;
            REQUIRE(Region::ForPlayer().holdFormID == kFalkreathHold);
        }
    }

    SECTION("when the player singleton is unavailable")
    {
        g_gridAnswer = 0;
        engine.player.present = false;

        SECTION("should report no hold rather than crash")
        {
            REQUIRE(Region::ForPlayer().holdFormID == 0);
        }
    }
}
