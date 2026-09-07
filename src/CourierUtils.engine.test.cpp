#include <CourierUtils.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

// Mocked-engine tests for the vanilla WICourier resolution.
//
// Pure engine access: the module looks the WICourier quest up by editor ID,
// finds its `Container` alias, falls back to the `WICourierContainerRef` REFR,
// and counts items in whichever staging container it landed on. Nothing here
// could be tested without standing in for the engine's form tables, which the
// harness reaches through data relocations.
//
// The module caches its resolution behind a one-shot flag, so these tests lean
// on `OnRevert()` between cases. That is not a test-only hook: the module was
// the one piece of cached state Plugin.cpp's revert handler did not reset,
// unlike the nine modules beside it, and the alias's live reference is
// per-save. Adding it made the cache correct across a load and testable at the
// same time.
//
// NOT COVERED: the branch where the `Container` alias resolves through
// `skyrim_cast<BGSRefAlias*>`. That cast compares the object's real MSVC RTTI
// against a relocated type descriptor, and the harness fabricates objects
// without RTTI. Everything downstream of a null alias — the fallback REFR and
// the no-container path — is covered.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace Courier = NarrativeEngine::CourierUtils;

    constexpr std::uint32_t kBookFormID = 0x0500082Au;

    // The cache is process-global and one-shot, so every case starts by
    // dropping whatever an earlier one resolved.
    struct FreshCourier
    {
        FreshCourier()
        {
            Courier::OnRevert();
        }
        ~FreshCourier()
        {
            Courier::OnRevert();
        }
    };
} // namespace

TEST_CASE("CourierUtils::ResolveCourierQuest", "[CourierUtils][engine]")
{
    // Happy path, re-run per leaf: WICourier is in the load order with both its
    // container alias and the fallback REFR. Each case removes a piece.
    EngineMock engine;
    FreshCourier fresh;

    SECTION("when WICourier is in the load order")
    {
        RE::TESQuest* quest = engine.AddCourierQuest(true, true);

        SECTION("should resolve the quest")
        {
            REQUIRE(Courier::ResolveCourierQuest() == quest);
        }

        SECTION("should resolve only once however often it is asked")
        {
            // The whole point of the cache: the second caller pays an atomic
            // load, not another pair of editor-ID lookups.
            REQUIRE(Courier::ResolveCourierQuest() == quest);
            REQUIRE(Courier::ResolveCourierQuest() == quest);
            REQUIRE(Courier::ResolveCourierQuest() == quest);
        }
    }

    SECTION("when WICourier is not in the load order")
    {
        SECTION("should resolve to nothing")
        {
            // Nothing was registered, so the editor-ID lookup finds no quest.
            REQUIRE(Courier::ResolveCourierQuest() == nullptr);
        }

        SECTION("should leave the courier container unresolved too")
        {
            (void)Courier::ResolveCourierQuest();
            REQUIRE(Courier::GetCourierContainerRef() == nullptr);
        }
    }

    SECTION("when the resolution is dropped on a revert")
    {
        RE::TESQuest* quest = engine.AddCourierQuest(true, true);
        REQUIRE(Courier::ResolveCourierQuest() == quest);

        SECTION("should resolve afresh next time")
        {
            // The behaviour the revert hook exists for. Without it a pointer
            // cached under one save keeps being handed out under the next.
            Courier::OnRevert();
            REQUIRE(Courier::ResolveCourierQuest() == quest);
        }

        SECTION("should forget the container it had found")
        {
            Courier::OnRevert();
            REQUIRE(Courier::GetCourierContainerRef() == nullptr);
        }
    }
}

TEST_CASE("CourierUtils::GetCourierContainerRef", "[CourierUtils][engine]")
{
    EngineMock engine;
    FreshCourier fresh;

    SECTION("when only the fallback reference resolves")
    {
        // No `Container` alias on the quest, which is what a stripped or
        // mod-replaced WICourier looks like.
        (void)engine.AddCourierQuest(false, true);
        (void)Courier::ResolveCourierQuest();

        SECTION("should fall back to the WICourierContainerRef")
        {
            REQUIRE(Courier::GetCourierContainerRef() != nullptr);
        }
    }

    SECTION("when neither the alias nor the fallback resolves")
    {
        (void)engine.AddCourierQuest(false, false);
        (void)Courier::ResolveCourierQuest();

        SECTION("should resolve to nothing")
        {
            // Every dispatch rolls back from here, which is why resolution
            // warns loudly rather than failing silently at hand-off time.
            REQUIRE(Courier::GetCourierContainerRef() == nullptr);
        }
    }

    SECTION("when nothing has been resolved yet")
    {
        SECTION("should resolve to nothing")
        {
            REQUIRE(Courier::GetCourierContainerRef() == nullptr);
        }
    }
}

TEST_CASE("CourierUtils::GetCourierInventoryCount", "[CourierUtils][engine]")
{
    // Happy path: a resolved courier container holding three copies of one
    // book. Each case removes a piece of that chain.
    EngineMock engine;
    FreshCourier fresh;
    (void)engine.AddCourierQuest(false, true);
    (void)engine.AddBook(kBookFormID);
    (void)Courier::ResolveCourierQuest();

    SECTION("when the book is in the container")
    {
        SECTION("should report the absolute count")
        {
            // An absolute total, not a signed delta: the count comes from the
            // base container contents plus the inventory-changes delta, and a
            // caller comparing it against an expected number needs the total.
            REQUIRE(Courier::GetCourierInventoryCount(kBookFormID) == 3);
        }

        SECTION("should ask the container about it")
        {
            (void)Courier::GetCourierInventoryCount(kBookFormID);
            REQUIRE(engine.courier.getInventoryCountsCalls == 1);
        }
    }

    SECTION("when the book is not in the container")
    {
        engine.courier.inventoryCount = -1;

        SECTION("should report zero")
        {
            REQUIRE(Courier::GetCourierInventoryCount(kBookFormID) == 0);
        }
    }

    SECTION("when the form id is zero")
    {
        SECTION("should report zero")
        {
            REQUIRE(Courier::GetCourierInventoryCount(0) == 0);
        }

        SECTION("should not ask the container anything")
        {
            // The guard comes first, so a null form id never becomes an
            // inventory walk over a container full of items.
            (void)Courier::GetCourierInventoryCount(0);
            REQUIRE(engine.courier.getInventoryCountsCalls == 0);
        }
    }

    SECTION("when the book form does not resolve")
    {
        SECTION("should report zero")
        {
            REQUIRE(Courier::GetCourierInventoryCount(kBookFormID + 1) == 0);
        }
    }

    SECTION("when there is no courier container")
    {
        Courier::OnRevert();

        SECTION("should report zero")
        {
            REQUIRE(Courier::GetCourierInventoryCount(kBookFormID) == 0);
        }

        SECTION("should not ask anything for an inventory")
        {
            (void)Courier::GetCourierInventoryCount(kBookFormID);
            REQUIRE(engine.courier.getInventoryCountsCalls == 0);
        }
    }
}
