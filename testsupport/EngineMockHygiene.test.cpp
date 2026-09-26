#include <EngineMock.h>

#include <StuckRecovery.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

// Tests for the harness rather than for anything it stands in for.
//
// EngineMock fabricates references out of pooled storage, and the pool hands
// the same addresses to the next mock. Four side tables are keyed off those
// addresses — what a cell holds, what a handle resolves to, whether an
// extra-data list carries teleport data, and which cell the save files a
// reference in — so a mock that does not reset them starts life holding the
// previous one's answers about storage it has just been given back.
//
// That is not theoretical. It has now produced three failures in this suite:
// an editor-location table that outlived its mock; a reference pool that did
// the same, surfacing as `VisitArrivalPoint finds a visitor who is not loaded`
// failing roughly once in twenty-five runs; and navmesh, which stayed attached
// to cells and quietly handed the next test the last one's terrain. Catch2
// re-runs a TEST_CASE body once per leaf SECTION in the same process, so a
// case with several sections builds several mocks back to back and is exactly
// where this bites.
//
// The navmesh one is worth its own note, because it hid for so long: every
// existing case laid a patch covering at least as much ground as the one
// before, so a leaked patch was always a subset of the live one and changed no
// answer. It surfaced the first time a case needed navmesh to be ABSENT
// somewhere — a corridor test asking whether there was a gap between two
// islands, which there was not, because a previous section had paved it.
//
// A flake like that costs far more to chase than the invariant costs to pin,
// and the invariant is cheap: a mock starts with nothing in it.

namespace
{
    using NarrativeEngine::Testing::EngineMock;

    constexpr std::uint32_t kWorld = 0x00E10001u;
    constexpr std::uint32_t kFarWorld = 0x00E10002u;
    constexpr std::uint32_t kDoor = 0x00E10010u;

    std::size_t CountReferencesIn(RE::TESObjectCELL* cell)
    {
        std::size_t seen = 0;
        cell->ForEachReference([&](RE::TESObjectREFR*) {
            ++seen;
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return seen;
    }
} // namespace

TEST_CASE("EngineMock hands the next test an empty reference pool", "[EngineMock][engine]")
{
    // Worldspaces and their cells are long-lived record data that a save does
    // not reset, so the world pool deliberately survives a mock and this
    // pointer stays good across both scopes. That is what makes the question
    // askable: the cell is the same cell, and the only thing that should have
    // changed is what is standing in it.
    RE::TESObjectCELL* street = nullptr;

    {
        EngineMock engine;
        auto* city = engine.AddWorldSpace(kWorld);
        auto* elsewhere = engine.AddWorldSpace(kFarWorld);
        street = engine.AddExteriorCell(city, 0, 0, nullptr);
        auto* outside = engine.AddExteriorCell(elsewhere, 0, 0, nullptr);

        auto* door = engine.AddLoadDoor(street, kDoor, outside, RE::NiPoint3{1.0f, 2.0f, 3.0f});
        engine.SetSaveParentCell(door, street);

        REQUIRE(CountReferencesIn(street) == 1);
        REQUIRE(door->extraList.GetByType<RE::ExtraTeleport>() != nullptr);
    }

    {
        EngineMock engine;

        SECTION("should not report the last test's references as standing in a cell")
        {
            // Left uncleared, a search of the loaded grid walks references
            // belonging to a mock that no longer exists — and a module that
            // asks every reference in the grid whether it is a load door,
            // the way the city-gate search does, gets told yes.
            REQUIRE(CountReferencesIn(street) == 0);
        }

        SECTION("should give a fresh reference no teleport data of its own")
        {
            // The teleport table is keyed by the address of the extra-data
            // list, which is interior to pooled reference storage, so this is
            // the entry most likely to land under a brand-new reference.
            auto* plain = engine.AddReference(street, 0x00E10020u, RE::NiPoint3{});
            REQUIRE(plain->extraList.GetByType<RE::ExtraTeleport>() == nullptr);
        }

        SECTION("should give a fresh reference no save cell it was never told about")
        {
            // GetSaveParentCell is the fallback every unloaded-reference read
            // in this project now goes through, so a stale entry here answers
            // a question the test never set up.
            auto* plain = engine.AddReference(street, 0x00E10021u, RE::NiPoint3{});
            plain->parentCell = nullptr;
            REQUIRE(plain->GetSaveParentCell() == nullptr);
        }
    }
}

TEST_CASE("EngineMock hands the next test bare ground", "[EngineMock][engine]")
{
    // Navmesh storage is leaked on purpose — every piece of it frees through a
    // relocation or a fabricated vtable — so what has to be reset is which
    // meshes each CELL offers. Cells live in the world pool, which outlives a
    // mock deliberately, and a cell that keeps its meshes hands the next test
    // terrain it never laid.
    {
        EngineMock engine;
        engine.world.cellIsInterior = false;
        engine.terrain.landHeight = 0.0f;
        engine.AddNavmeshPatch(engine.GroundCell(), 0x00E11001u, -5000.0f, -5000.0f, 5000.0f, 5000.0f, 0.0f);
        REQUIRE(NarrativeEngine::StuckRecovery::IsOnNavmesh(RE::NiPoint3{0.0f, 0.0f, 8.0f}));
    }

    {
        EngineMock engine;
        engine.world.cellIsInterior = false;
        engine.terrain.landHeight = 0.0f;

        SECTION("should not report the last test's navmesh under a position")
        {
            // A test that needs navmesh to be absent somewhere -- which is the
            // only way to ask whether two patches of it are joined -- gets the
            // wrong answer otherwise, and gets it silently.
            REQUIRE_FALSE(NarrativeEngine::StuckRecovery::IsOnNavmesh(RE::NiPoint3{0.0f, 0.0f, 8.0f}));
        }
    }
}
