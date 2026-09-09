#include <HoldGrid.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

// Tests for the cell-to-hold partition.
//
// "Which hold is this?" has no answer in the game's data for most of Skyrim.
// Cells carry a location only where the designers put one, and the parent-chain
// walk that resolves a location to a hold therefore comes up empty across most
// of the wilderness — which is precisely the ground a traveller crosses. The
// grid closes that gap by taking the cells that DO name a hold as seeds and
// flooding outward from all of them at once, so every reachable coordinate ends
// up attributed to the nearest one.
//
// Three things about the flood are worth pinning.
//
// It runs from every seed simultaneously and the first arrival wins, which
// under four-neighbour steps approximates a Voronoi partition: the boundary
// between two holds falls where the walking distance to each is equal. Filling
// one hold to exhaustion before starting the next would instead give the first
// hold processed the whole province.
//
// Small isolated seed clusters are dropped before the flood begins. Vanilla
// data has a handful of mis-tagged location groups, and a single stray cell
// naming the wrong hold seeds a front that propagates until it meets one going
// the other way — turning one bad record into a wedge of wrongly attributed
// wilderness.
//
// And a cell without a hold of its own is not skipped, it is filled. That is
// the entire point: the answer for unclassified ground is the neighbour it is
// nearest to, not silence.
//
// ONE GRID PER PROCESS. Initialize is one-shot with no reset, so each test case
// builds its world once above the sections and the sections only ask questions
// of it. The suite runs through ctest, which gives every test case its own
// process.

namespace
{
    namespace HoldGrid = NarrativeEngine::HoldGrid;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr const char* kSettings = "[HoldGrid]\nbHoldGridDebugBitmap=0\n";

    constexpr std::uint32_t kTamriel = 0x0000003Cu;
    constexpr std::uint32_t kWhiterunHold = 0x00016BE4u;
    constexpr std::uint32_t kFalkreathHold = 0x0001680Fu;
    constexpr float kCellUnits = 4096.0f;

    // A hold, and a location inside it for cells to point at. Cells name a
    // location, not a hold: the chain from one to the other is what the builder
    // walks, and a test that pointed cells straight at the hold would never
    // exercise it.
    struct Hold
    {
        RE::BGSLocation* territory = nullptr;
        std::uint32_t formID = 0;
    };

    Hold MakeHold(EngineMock& engine, std::uint32_t formID, const char* name, const char* editorID)
    {
        (void)engine.AddKeyword("LocTypeHold");
        auto* hold = engine.AddLocation(formID, name, {"LocTypeHold"}, editorID);
        auto* territory = engine.AddLocation(formID + 0x00100000u, "Somewhere In It", {});
        engine.SetLocationParent(territory, hold);
        return Hold{territory, formID};
    }

    // A square block of cells all belonging to one hold. Blocks are three
    // across because the builder discards isolated clusters of three cells or
    // fewer, and a smaller seed would be pruned before the flood ever saw it.
    void SeedBlock(EngineMock& engine,
                   RE::TESWorldSpace* space,
                   const Hold& hold,
                   std::int16_t westCell,
                   std::int16_t southCell,
                   std::int16_t size = 3)
    {
        for (std::int16_t dx = 0; dx < size; ++dx) {
            for (std::int16_t dy = 0; dy < size; ++dy) {
                (void)engine.AddExteriorCell(space,
                                             static_cast<std::int16_t>(westCell + dx),
                                             static_cast<std::int16_t>(southCell + dy),
                                             hold.territory);
            }
        }
    }
} // namespace

TEST_CASE("HoldGrid partitions a worldspace between its holds", "[HoldGrid][engine]")
{
    // Two hold blocks side by side, with the boundary falling exactly between
    // cell -1 and cell 0. Everything else is unclassified ground for the flood
    // to attribute.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    const auto falkreath = MakeHold(engine, kFalkreathHold, "Falkreath Hold", "FalkreathHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    SeedBlock(engine, space, whiterun, -3, -1);
    SeedBlock(engine, space, falkreath, 0, -1);
    HoldGrid::Initialize();

    SECTION("when a cell named a hold of its own")
    {
        SECTION("should answer with that hold")
        {
            REQUIRE(HoldGrid::LookupCell(space, -1, 0) == kWhiterunHold);
            REQUIRE(HoldGrid::LookupCell(space, 0, 0) == kFalkreathHold);
        }
    }

    SECTION("when a cell named nothing at all")
    {
        SECTION("should answer with the hold it is nearest to")
        {
            // This is the whole reason the grid exists. Most of the province
            // has no location record, and leaving it unattributed puts the
            // Director's geography checks out of action across every stretch
            // of road between two towns.
            REQUIRE(HoldGrid::LookupCell(space, -20, 0) == kWhiterunHold);
            REQUIRE(HoldGrid::LookupCell(space, 20, 0) == kFalkreathHold);
        }

        SECTION("should fill from every hold at once")
        {
            // Not one hold to exhaustion and then the next. A cell twenty
            // north of both blocks is equally far from each in the y
            // direction, so which one claims it is decided by the x distance
            // — which only holds if both fronts advanced together.
            REQUIRE(HoldGrid::LookupCell(space, -20, 20) == kWhiterunHold);
            REQUIRE(HoldGrid::LookupCell(space, 20, 20) == kFalkreathHold);
        }
    }

    SECTION("when a cell lies outside the worldspace")
    {
        SECTION("should answer with nothing")
        {
            // The flood is bounded by the map data, so coordinates past the
            // edge were never filled. Answering for them would attribute
            // ground that does not exist.
            REQUIRE(HoldGrid::LookupCell(space, 1000, 1000) == 0);
        }
    }

    SECTION("when there is no worldspace to ask about")
    {
        SECTION("should answer with nothing")
        {
            REQUIRE(HoldGrid::LookupCell(nullptr, 0, 0) == 0);
        }
    }

    SECTION("when a world position is given instead of a cell")
    {
        SECTION("should answer for the cell containing it")
        {
            REQUIRE(HoldGrid::LookupWorldPosition(space, 1.5f * kCellUnits, 0.0f) == kFalkreathHold);
        }

        SECTION("should round west and south rather than towards zero")
        {
            // A position a hundred units west of the origin is in cell -1, not
            // cell 0. Truncating instead of flooring puts the whole strip
            // between x=-4096 and x=0 in the wrong cell, and here that is the
            // wrong hold as well.
            REQUIRE(HoldGrid::LookupWorldPosition(space, -100.0f, -100.0f) == kWhiterunHold);
        }
    }

    SECTION("when the build is asked for a second time")
    {
        HoldGrid::Initialize();

        SECTION("should leave the grid alone")
        {
            // It runs at kDataLoaded and the partition is const afterwards.
            REQUIRE(HoldGrid::LookupCell(space, -1, 0) == kWhiterunHold);
        }
    }
}

TEST_CASE("HoldGrid drops a stray seed", "[HoldGrid][engine]")
{
    // A block of Whiterun, and one lone Falkreath cell out in the middle of it.
    // Vanilla has a handful of these — a location group tagged to the wrong
    // hold — and each one would otherwise seed a front that pushes outward
    // until it meets one coming the other way.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    const auto falkreath = MakeHold(engine, kFalkreathHold, "Falkreath Hold", "FalkreathHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    SeedBlock(engine, space, whiterun, -3, -1);
    // Two cells rather than one, so that the cells of the cluster are company
    // for each other and the isolation test has to discount them.
    (void)engine.AddExteriorCell(space, 20, 20, falkreath.territory);
    (void)engine.AddExteriorCell(space, 20, 21, falkreath.territory);
    HoldGrid::Initialize();

    SECTION("when the stray has no company outside itself")
    {
        SECTION("should give its cells to the hold around it")
        {
            // A cluster is not company for itself. Counting its own members
            // would keep every stray that happened to span two cells, which is
            // most of the mis-tagged groups in the vanilla data.
            REQUIRE(HoldGrid::LookupCell(space, 20, 20) == kWhiterunHold);
            REQUIRE(HoldGrid::LookupCell(space, 20, 21) == kWhiterunHold);
        }

        SECTION("should leave nothing of it behind")
        {
            // The ground beside it too: a stray that survived would not stop
            // at its own cells, it would claim everything its front reached.
            REQUIRE(HoldGrid::LookupCell(space, 25, 25) == kWhiterunHold);
        }
    }
}

TEST_CASE("HoldGrid keeps a small cluster with company", "[HoldGrid][engine]")
{
    // Two small Falkreath clusters a few cells apart. Neither is big enough to
    // survive on its own, and together they are what a genuine outlying
    // settlement looks like — so both stay.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    const auto falkreath = MakeHold(engine, kFalkreathHold, "Falkreath Hold", "FalkreathHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    SeedBlock(engine, space, whiterun, -10, -1);
    (void)engine.AddExteriorCell(space, 20, 20, falkreath.territory);
    (void)engine.AddExteriorCell(space, 20, 21, falkreath.territory);
    (void)engine.AddExteriorCell(space, 23, 20, falkreath.territory);
    (void)engine.AddExteriorCell(space, 23, 21, falkreath.territory);
    HoldGrid::Initialize();

    SECTION("when another cluster of the same hold is within reach")
    {
        SECTION("should keep both")
        {
            // The isolation test is about company, not size: a cluster with a
            // same-hold neighbour a few cells away is a real outpost, and
            // dropping it would hand a settlement to the hold next door.
            REQUIRE(HoldGrid::LookupCell(space, 20, 20) == kFalkreathHold);
            REQUIRE(HoldGrid::LookupCell(space, 23, 21) == kFalkreathHold);
        }
    }
}

TEST_CASE("HoldGrid seeds from nothing it cannot classify", "[HoldGrid][engine]")
{
    // Three kinds of cell that name no hold, and nothing else in the world.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");

    // A block of interiors, whose locations chain to a hold perfectly well but
    // whose coordinates mean nothing: every interior in the game reports the
    // same handful of them. A block rather than one cell, so that the answer
    // does not depend on the pruning of small clusters as well.
    for (std::int16_t dx = 0; dx < 3; ++dx) {
        for (std::int16_t dy = 0; dy < 3; ++dy) {
            auto* interior = engine.AddExteriorCell(space, dx, dy, whiterun.territory);
            engine.SetCellInterior(interior, true);
        }
    }
    // A cell with no location at all, which is most of the wilderness.
    (void)engine.AddExteriorCell(space, 10, 0, nullptr);
    // A cell whose location chain never reaches a hold — a scripted dungeon,
    // or anything a mod added without filing it under a jarl.
    auto* unfiled = engine.AddLocation(0x00A9F001u, "Nowhere", {});
    (void)engine.AddExteriorCell(space, 11, 0, unfiled);
    HoldGrid::Initialize();

    SECTION("when nothing in the world names a hold")
    {
        SECTION("should build no grid at all")
        {
            // With no seed there is nothing to flood from, and inventing an
            // answer would attribute the whole worldspace to whatever was
            // guessed.
            REQUIRE(HoldGrid::LookupCell(space, 1, 1) == 0);
            REQUIRE(HoldGrid::LookupCell(space, 10, 0) == 0);
            REQUIRE(HoldGrid::LookupCell(space, 11, 0) == 0);
        }
    }
}

TEST_CASE("HoldGrid skips a worldspace with no map", "[HoldGrid][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    SeedBlock(engine, space, whiterun, -3, -1);
    // Written through the worldspace directly: every fabricated one gets bounds
    // wide enough for a test grid, and this is a worldspace whose map data has
    // the two corners the wrong way round.
    space->worldMapData.nwCellX = 64;
    space->worldMapData.seCellX = -64;
    HoldGrid::Initialize();

    SECTION("when the corners are the wrong way round")
    {
        SECTION("should leave it alone")
        {
            // Bounds that do not describe a rectangle are bounds nothing can
            // be flooded within, and seeding from cells the flood can never
            // reach would leave a scatter of lone attributed cells in a
            // worldspace that is otherwise blank.
            REQUIRE(HoldGrid::LookupCell(space, -1, 0) == 0);
        }
    }
}

TEST_CASE("HoldGrid gives up without the keyword", "[HoldGrid][engine]")
{
    // LocTypeHold is what marks a location as a hold, and it is added by a mod
    // rather than by the base game, so an install without that mod reaches
    // here.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* hold = engine.AddLocation(kWhiterunHold, "Whiterun Hold", {}, "WhiterunHoldLocation");
    auto* territory = engine.AddLocation(0x00A9F002u, "Somewhere In It", {});
    engine.SetLocationParent(territory, hold);
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    for (std::int16_t dx = 0; dx < 3; ++dx) {
        for (std::int16_t dy = 0; dy < 3; ++dy) {
            (void)engine.AddExteriorCell(space, dx, dy, territory);
        }
    }
    HoldGrid::Initialize();

    SECTION("when nothing can be recognised as a hold")
    {
        SECTION("should build no grid")
        {
            REQUIRE(HoldGrid::LookupCell(space, 1, 1) == 0);
        }
    }
}

TEST_CASE("HoldGrid gives up without the data handler", "[HoldGrid][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    SeedBlock(engine, space, whiterun, -3, -1);
    engine.world.dataHandlerPresent = false;
    HoldGrid::Initialize();

    SECTION("when the load order is not up yet")
    {
        SECTION("should build no grid")
        {
            REQUIRE(HoldGrid::LookupCell(space, -1, 0) == 0);
        }
    }
}

TEST_CASE("HoldGrid answers for where the player is", "[HoldGrid][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    SeedBlock(engine, space, whiterun, -3, -1);
    HoldGrid::Initialize();

    SECTION("when the player is standing outdoors")
    {
        engine.StandPlayerInCell(space, -1, 0);

        SECTION("should answer with the hold there")
        {
            REQUIRE(HoldGrid::LookupPlayer() == kWhiterunHold);
        }
    }

    SECTION("when the player is indoors")
    {
        engine.StandPlayerInCell(space, -1, 0);
        engine.world.cellIsInterior = true;

        SECTION("should answer with nothing")
        {
            // Interiors have no grid coordinates. The caller is expected to
            // fall back to the location chain, which does work indoors.
            REQUIRE(HoldGrid::LookupPlayer() == 0);
        }
    }

    SECTION("when the player is in no worldspace")
    {
        SECTION("should answer with nothing")
        {
            // Which is the state before a save has finished loading.
            REQUIRE(HoldGrid::LookupPlayer() == 0);
        }
    }

    SECTION("when there is no player")
    {
        engine.player.present = false;

        SECTION("should answer with nothing")
        {
            REQUIRE(HoldGrid::LookupPlayer() == 0);
        }
    }
}

TEST_CASE("HoldGrid draws its partition on request", "[HoldGrid][engine]")
{
    // The image is how the partition has been checked against the map, and the
    // only way anyone has seen what the flood actually produced.
    const std::filesystem::path logDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};
    const auto bitmap = logDir / "NarrativeEngine_HoldGrid_Tamriel.bmp";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    std::filesystem::remove(bitmap, ec);

    EngineMock engine;
    const ConfiguredSettings settings{"[HoldGrid]\nbHoldGridDebugBitmap=1\n"};
    const auto whiterun = MakeHold(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    const auto falkreath = MakeHold(engine, kFalkreathHold, "Falkreath Hold", "FalkreathHoldLocation");
    auto* space = engine.AddWorldSpace(kTamriel, "Tamriel");
    SeedBlock(engine, space, whiterun, -3, -1);
    SeedBlock(engine, space, falkreath, 0, -1);
    HoldGrid::Initialize();

    SECTION("when the dump is turned on")
    {
        SECTION("should write one image per worldspace")
        {
            REQUIRE(std::filesystem::exists(bitmap));
        }
    }
}
