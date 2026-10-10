#include <LocationKeywords.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Mocked-engine tests for the vanilla location-keyword predicates.
//
// These four booleans decide where a beat may and may not act: an ambush is
// refused in a settlement, a visit is refused in a jail, and so on. Getting one
// wrong does not crash anything — it just puts a bandit ambush in the middle of
// Whiterun — which is precisely why the lists and the ancestor walk are worth
// pinning rather than eyeballing.
//
// The module resolves its whole keyword table once per process behind a
// function-local static, which is right for the game (keyword forms are static
// vanilla data) but means the harness must keep keywords alive across
// EngineMock lifetimes. It does; see EngineMock::AddKeyword.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace Keywords = NarrativeEngine::LocationKeywords;

    constexpr std::uint32_t kChildFormID = 0x00018A56u;
    constexpr std::uint32_t kParentFormID = 0x00018A57u;

    // Registers every editor ID the module will look up. The resolution is
    // one-shot per process, so whichever case runs first fixes the table for
    // the whole run — it therefore has to see all of them.
    void RegisterEveryKeyword(EngineMock& engine)
    {
        for (const auto& list :
             {std::vector<std::string_view>{Keywords::kSafe.begin(), Keywords::kSafe.end()},
              std::vector<std::string_view>{Keywords::kDangerous.begin(), Keywords::kDangerous.end()},
              std::vector<std::string_view>{Keywords::kOccupied.begin(), Keywords::kOccupied.end()},
              std::vector<std::string_view>{Keywords::kVisitHostileExtras.begin(),
                                            Keywords::kVisitHostileExtras.end()}}) {
            for (const auto edid : list)
                (void)engine.AddKeyword(edid);
        }
    }
} // namespace

TEST_CASE("LocationKeywords::IsSafe", "[LocationKeywords][engine]")
{
    // Happy path, re-run per leaf: every vanilla keyword resolvable, and a
    // location carrying none of them. Each case gives the location the keyword
    // it is about.
    EngineMock engine;
    RegisterEveryKeyword(engine);

    SECTION("when the location carries a safe keyword")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Whiterun", {"LocTypeCity"});

        SECTION("should say it is safe")
        {
            REQUIRE(Keywords::IsSafe(loc));
        }

        SECTION("should not say it is dangerous")
        {
            // The sets are independent lists rather than a partition, so a
            // location can be neither, and a safe one usually is.
            REQUIRE_FALSE(Keywords::IsDangerous(loc));
        }
    }

    SECTION("when the location carries no keyword at all")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Unmarked Clearing", {});

        SECTION("should say it is not safe")
        {
            REQUIRE_FALSE(Keywords::IsSafe(loc));
        }
    }

    SECTION("when only an ancestor carries the keyword")
    {
        // The reason the walk exists: WhiterunStablesExterior has no LocType
        // of its own and inherits its parent's, which is how vanilla quest
        // conditions read these too.
        auto* parent = engine.AddLocation(kParentFormID, "Whiterun", {"LocTypeCity"});
        auto* child = engine.AddLocation(kChildFormID, "Whiterun Stables", {});
        engine.SetLocationParent(child, parent);

        SECTION("should inherit the classification")
        {
            REQUIRE(Keywords::IsSafe(child));
        }
    }

    SECTION("when the location is null")
    {
        SECTION("should say it is not safe")
        {
            REQUIRE_FALSE(Keywords::IsSafe(nullptr));
        }
    }

    SECTION("when the parent chain forms a cycle")
    {
        // Malformed mod data. The walk is depth-capped rather than
        // cycle-detecting, so this must terminate rather than spin.
        auto* first = engine.AddLocation(kParentFormID, "First", {});
        auto* second = engine.AddLocation(kChildFormID, "Second", {});
        engine.SetLocationParent(first, second);
        engine.SetLocationParent(second, first);

        SECTION("should terminate and say it is not safe")
        {
            REQUIRE_FALSE(Keywords::IsSafe(first));
        }
    }
}

TEST_CASE("LocationKeywords::IsDangerous", "[LocationKeywords][engine]")
{
    EngineMock engine;
    RegisterEveryKeyword(engine);

    SECTION("when the location is a lair")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Bleak Falls Barrow", {"LocTypeDraugrCrypt"});

        SECTION("should say it is dangerous")
        {
            REQUIRE(Keywords::IsDangerous(loc));
        }
    }

    SECTION("when the keyword is a location SET rather than a type")
    {
        // The dangerous list mixes LocSet* and LocType* prefixes, which is a
        // real property of vanilla's tagging rather than an oversight.
        auto* loc = engine.AddLocation(kChildFormID, "Some Cave", {"LocSetCave"});

        SECTION("should still say it is dangerous")
        {
            REQUIRE(Keywords::IsDangerous(loc));
        }
    }

    SECTION("when the location is safe")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Riverwood", {"LocTypeTown"});

        SECTION("should say it is not dangerous")
        {
            REQUIRE_FALSE(Keywords::IsDangerous(loc));
        }
    }
}

TEST_CASE("LocationKeywords::IsOccupied", "[LocationKeywords][engine]")
{
    EngineMock engine;
    RegisterEveryKeyword(engine);

    SECTION("when the location carries a civil-war tag")
    {
        // Unlike the other two lists these are not authorial classifications
        // but tags vanilla's own quest scripts attach, which is why the header
        // warns they may move during a playthrough.
        auto* loc = engine.AddLocation(kChildFormID, "Whiterun", {"CWEventHappening"});

        SECTION("should say it is occupied")
        {
            REQUIRE(Keywords::IsOccupied(loc));
        }
    }

    SECTION("when the location carries a world-interaction tag")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Somewhere", {"WIDragonAttacked"});

        SECTION("should say it is occupied")
        {
            REQUIRE(Keywords::IsOccupied(loc));
        }
    }

    SECTION("when the location is an empty wilderness clearing")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Unmarked Clearing", {});

        SECTION("should say it is not occupied")
        {
            // Where an ambush belongs, and the only kind of place it does.
            REQUIRE_FALSE(Keywords::IsOccupied(loc));
        }
    }
}

TEST_CASE("LocationKeywords::IsVisitHostile", "[LocationKeywords][engine]")
{
    // The union of two lists: everything dangerous, plus a short set of
    // civilised places where a stranger walking up to talk would still read as
    // jarring. Kept apart so the dangerous table stays about combat.
    EngineMock engine;
    RegisterEveryKeyword(engine);

    SECTION("when the location is dangerous")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Bandit Camp", {"LocTypeBanditCamp"});

        SECTION("should say a visit would be hostile")
        {
            REQUIRE(Keywords::IsVisitHostile(loc));
        }
    }

    SECTION("when the location is only on the extras list")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Dragonsreach Dungeon", {"LocTypeJail"});

        SECTION("should say a visit would be hostile")
        {
            // A jail is civilised, so IsDangerous says no. Without the extras
            // list an NPC would happily wander in to make conversation.
            REQUIRE(Keywords::IsVisitHostile(loc));
        }

        SECTION("should not be dangerous in its own right")
        {
            REQUIRE_FALSE(Keywords::IsDangerous(loc));
        }

        SECTION("should still count as safe")
        {
            // LocTypeJail is on both the safe and the extras list, which is the
            // point: safe for an ambush check, hostile for a visit check.
            REQUIRE(Keywords::IsSafe(loc));
        }
    }

    SECTION("when the location is an ordinary town")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Riverwood", {"LocTypeTown"});

        SECTION("should say a visit would be fine")
        {
            REQUIRE_FALSE(Keywords::IsVisitHostile(loc));
        }
    }
}

TEST_CASE("LocationKeywords degrades when a keyword will not resolve", "[LocationKeywords][engine]")
{
    // The module documents that it fails OPEN: a keyword that does not resolve
    // is skipped rather than treated as a match, because failing closed would
    // silently block every action everywhere instead of only at the affected
    // location.
    //
    // The resolution is one-shot per process, so this cannot be provoked by
    // withholding a registration — whichever case ran first already resolved
    // the table. What is checkable is the consequence: an unresolved entry
    // contributes nothing, which is the same as a location simply not carrying
    // that keyword.
    EngineMock engine;
    RegisterEveryKeyword(engine);

    SECTION("when a location carries a keyword outside every list")
    {
        auto* loc = engine.AddLocation(kChildFormID, "Somewhere", {"LocTypeNotOnAnyListOfOurs"});

        SECTION("should match none of the predicates")
        {
            REQUIRE_FALSE(Keywords::IsSafe(loc));
            REQUIRE_FALSE(Keywords::IsDangerous(loc));
            REQUIRE_FALSE(Keywords::IsOccupied(loc));
            REQUIRE_FALSE(Keywords::IsVisitHostile(loc));
        }
    }
}
