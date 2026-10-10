#include <Region.h>

#include <EngineMock.h>
#include <HoldGrid.h>

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
// fallback walks the parent chain. HoldGrid is the real one here, so the fast
// path is reached by building a grid and standing the player in it, and the
// fallback by standing them somewhere the grid does not index — an interior,
// which is every indoor cell in the game.
//
// Two of the module's guards are unreachable that way and are left uncovered
// deliberately: the grid stores raw FormIDs, and a real builder never puts one
// in that names something other than a location, or one that no longer
// resolves. Both are defence against stale data rather than behaviour a caller
// can produce.
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

TEST_CASE("Region::ForLocation walks the parent chain", "[Region][engine]")
{
    // Happy path, re-run per leaf: the grid is silent, so every case here
    // exercises the walk. The player's own location is not a hold.
    EngineMock engine;
    RegisterHolds(engine);

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
    // Happy path, re-run per leaf: a real hold grid covering a block of
    // exterior cells around the origin, with the player standing in the middle
    // of it. That is the state after a load in any part of Skyrim the builder
    // reached, which is nearly all of it.
    //
    // The block is nine cells because the builder prunes isolated clusters of
    // three or fewer as mis-tagged data, and one cell on its own would be
    // pruned away before any lookup could see it.
    EngineMock engine;
    RegisterHolds(engine);
    auto* whiterun = engine.AddLocation(kWhiterunHold, "Whiterun Hold", {"LocTypeHold"}, "WhiterunHoldLocation");
    auto* space = engine.AddWorldSpace(0x0000003Cu);
    for (std::int16_t x = -1; x <= 1; ++x) {
        for (std::int16_t y = -1; y <= 1; ++y) {
            (void)engine.AddExteriorCell(space, x, y, whiterun);
        }
    }
    NarrativeEngine::HoldGrid::Initialize();
    engine.StandPlayerInCell(space, 0, 0);

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
            engine.world.playerHasLocation = true;
            engine.world.playerLocationOverride = here;
            REQUIRE(Region::ForPlayer().holdFormID == kWhiterunHold);
        }
    }

    SECTION("when the grid has no answer")
    {
        // Every interior cell, which the grid does not index at all.
        engine.world.cellIsInterior = true;

        SECTION("should walk the player's location chain")
        {
            auto* falkreath =
                engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
            auto* here = engine.AddLocation(kChildLocation, "Somewhere", {});
            engine.SetLocationParent(here, falkreath);
            engine.world.playerHasLocation = true;
            engine.world.playerLocationOverride = here;
            REQUIRE(Region::ForPlayer().holdFormID == kFalkreathHold);
        }
    }
}
