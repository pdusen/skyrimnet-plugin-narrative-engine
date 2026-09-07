#include <FactionDesignationUtils.h>

#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

// Mocked-engine tests for the rank-based sender designation.
//
// The module uses a faction as a one-actor-at-a-time selector rather than as
// faction logic: exactly one loaded actor sits at the designated rank, and an
// alias condition of `GetFactionRank marker >= designatedRank` binds the
// Find-Matching-Reference fill to that actor. Everything it does is therefore
// about WHO ends up at which rank, which is why the harness records every
// AddToFaction call rather than only the resulting ranks.
//
// Running it needs a fair slice of the engine: ProcessLists' loaded-actor
// lists, actor handles resolving back to actors, and faction ranks. All of it
// is link-time mockable, so the production source compiles here unchanged.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace Faction = NarrativeEngine::FactionDesignationUtils;

    constexpr std::int8_t kDesignated = 2;
    constexpr std::int8_t kCandidate = 1;
    constexpr std::string_view kLogTag = "TestBeat";

    constexpr std::uint32_t kFactionFormID = 0x05000800u;
    constexpr std::uint32_t kSenderFormID = 0x0001A6A0u;
    constexpr std::uint32_t kStragglerFormID = 0x0001A6A1u;
    constexpr std::uint32_t kBystanderFormID = 0x0001A6A2u;
} // namespace

TEST_CASE("FactionDesignationUtils::PromoteToDesignated", "[FactionDesignationUtils][engine]")
{
    // Happy path, re-run per leaf: a marker faction, a sender not yet in it,
    // and a bystander loaded nearby who has nothing to do with the beat.
    EngineMock engine;
    RE::TESFaction* faction = engine.AddFaction(kFactionFormID);
    RE::Actor* sender = engine.AddLoadedActor(kSenderFormID);
    RE::Actor* bystander = engine.AddLoadedActor(kBystanderFormID);

    SECTION("when the sender is not yet in the faction")
    {
        Faction::PromoteToDesignated(faction, sender, kDesignated, kCandidate, kLogTag);

        SECTION("should put the sender at the designated rank")
        {
            REQUIRE(engine.FactionRank(sender, faction) == kDesignated);
        }

        SECTION("should leave everyone else alone")
        {
            // A bystander who was never in the faction must not be dragged
            // into it: the alias condition is a rank test, and adding someone
            // at any rank puts them in the running.
            REQUIRE(engine.FactionRank(bystander, faction) == -1);
        }
    }

    SECTION("when the sender is already at the designated rank")
    {
        engine.SetFactionRank(sender, faction, kDesignated);
        engine.factions.addToFactionCalls.clear();
        Faction::PromoteToDesignated(faction, sender, kDesignated, kCandidate, kLogTag);

        SECTION("should leave the rank where it is")
        {
            REQUIRE(engine.FactionRank(sender, faction) == kDesignated);
        }

        SECTION("should not touch the faction at all")
        {
            // A no-op by contract. Re-adding would rewrite the actor's
            // ExtraFactionChanges record for no reason on every retry.
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }

    SECTION("when a straggler is still designated from a previous dispatch")
    {
        // The state a crash inside the compose window leaves behind: the
        // previous sender never got demoted, so two actors would satisfy the
        // alias condition and the fill could bind to the wrong one.
        RE::Actor* straggler = engine.AddLoadedActor(kStragglerFormID);
        engine.SetFactionRank(straggler, faction, kDesignated);
        Faction::PromoteToDesignated(faction, sender, kDesignated, kCandidate, kLogTag);

        SECTION("should demote the straggler to the candidate rank")
        {
            REQUIRE(engine.FactionRank(straggler, faction) == kCandidate);
        }

        SECTION("should still promote the sender")
        {
            REQUIRE(engine.FactionRank(sender, faction) == kDesignated);
        }

        SECTION("should leave exactly one actor at the designated rank")
        {
            // The whole point of the sweep. More than one and the
            // Find-Matching-Reference fill picks whichever it reaches first.
            int designatedCount = 0;
            for (auto* actor : {sender, straggler, bystander}) {
                if (engine.FactionRank(actor, faction) >= kDesignated)
                    ++designatedCount;
            }
            REQUIRE(designatedCount == 1);
        }
    }

    SECTION("when the faction is missing")
    {
        SECTION("should do nothing")
        {
            Faction::PromoteToDesignated(nullptr, sender, kDesignated, kCandidate, kLogTag);
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }

    SECTION("when the sender is missing")
    {
        SECTION("should do nothing")
        {
            Faction::PromoteToDesignated(faction, nullptr, kDesignated, kCandidate, kLogTag);
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }
}

TEST_CASE("FactionDesignationUtils::SweepStaleDesignated", "[FactionDesignationUtils][engine]")
{
    EngineMock engine;
    RE::TESFaction* faction = engine.AddFaction(kFactionFormID);
    RE::Actor* target = engine.AddLoadedActor(kSenderFormID);
    RE::Actor* straggler = engine.AddLoadedActor(kStragglerFormID);

    SECTION("when a loaded actor sits at or above the designated rank")
    {
        engine.SetFactionRank(straggler, faction, kDesignated);
        Faction::SweepStaleDesignated(faction, target, kDesignated, kCandidate, kLogTag);

        SECTION("should demote it")
        {
            REQUIRE(engine.FactionRank(straggler, faction) == kCandidate);
        }
    }

    SECTION("when an actor sits above the designated rank")
    {
        // The comparison is `>=`, so a rank higher than designated is swept
        // too. A future beat using a wider rank scheme depends on that.
        engine.SetFactionRank(straggler, faction, static_cast<std::int8_t>(kDesignated + 5));
        Faction::SweepStaleDesignated(faction, target, kDesignated, kCandidate, kLogTag);

        SECTION("should demote it as well")
        {
            REQUIRE(engine.FactionRank(straggler, faction) == kCandidate);
        }
    }

    SECTION("when the actor is the sweep's own target")
    {
        engine.SetFactionRank(target, faction, kDesignated);
        Faction::SweepStaleDesignated(faction, target, kDesignated, kCandidate, kLogTag);

        SECTION("should leave it designated")
        {
            // Sweeping the incoming sender would undo the promotion it is
            // being run in aid of.
            REQUIRE(engine.FactionRank(target, faction) == kDesignated);
        }
    }

    SECTION("when an actor sits below the designated rank")
    {
        engine.SetFactionRank(straggler, faction, kCandidate);
        engine.factions.addToFactionCalls.clear();
        Faction::SweepStaleDesignated(faction, target, kDesignated, kCandidate, kLogTag);

        SECTION("should leave it alone")
        {
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }

    SECTION("when the faction is missing")
    {
        engine.SetFactionRank(straggler, faction, kDesignated);
        engine.factions.addToFactionCalls.clear();

        SECTION("should do nothing")
        {
            Faction::SweepStaleDesignated(nullptr, target, kDesignated, kCandidate, kLogTag);
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }
}

TEST_CASE("FactionDesignationUtils::DemoteToCandidate", "[FactionDesignationUtils][engine]")
{
    // Happy path: a sender currently designated, as every terminal path in a
    // beat finds them.
    EngineMock engine;
    RE::TESFaction* faction = engine.AddFaction(kFactionFormID);
    RE::Actor* sender = engine.AddLoadedActor(kSenderFormID);
    engine.SetFactionRank(sender, faction, kDesignated);
    engine.factions.addToFactionCalls.clear();

    SECTION("when the sender is designated")
    {
        SECTION("should return them to the candidate rank")
        {
            Faction::DemoteToCandidate(faction, sender, kCandidate, kLogTag);
            REQUIRE(engine.FactionRank(sender, faction) == kCandidate);
        }
    }

    SECTION("when the sender is already at the candidate rank")
    {
        engine.SetFactionRank(sender, faction, kCandidate);
        engine.factions.addToFactionCalls.clear();

        SECTION("should not touch the faction")
        {
            Faction::DemoteToCandidate(faction, sender, kCandidate, kLogTag);
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }

    SECTION("when the sender is not in the faction")
    {
        RE::Actor* outsider = engine.AddLoadedActor(kBystanderFormID);

        SECTION("should not add them to it")
        {
            // Demotion runs on every terminal path, including ones where the
            // promotion never happened. Adding here would leave an actor in
            // the marker faction that nothing will ever take out.
            Faction::DemoteToCandidate(faction, outsider, kCandidate, kLogTag);
            REQUIRE(engine.FactionRank(outsider, faction) == -1);
        }
    }

    SECTION("when the faction or sender is missing")
    {
        SECTION("should do nothing")
        {
            Faction::DemoteToCandidate(nullptr, sender, kCandidate, kLogTag);
            Faction::DemoteToCandidate(faction, nullptr, kCandidate, kLogTag);
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }
}
