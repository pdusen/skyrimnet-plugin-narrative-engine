#include <AliasWalkFilter.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

// Tests for the "is this NPC already spoken for" gate.
//
// This is what keeps NarrativeEngine from competing with authored content. An
// NPC filled into a running quest's alias is being puppeteered by somebody
// else's questline, and dispatching a letter or a visit to them means two
// systems driving one actor. The symptom is not a crash: it is a scene that
// stalls, or an NPC who abandons a vanilla quest step to walk across Skyrim.
//
// Three rules here are worth more than the rest.
//
// Self-exclusion is by ESP file rather than by quest EditorID, so a sender who
// is mid-visit is not flagged by their own visit quest. Getting that wrong
// makes every one of our own beats mark its own participants unavailable, and
// the pool empties itself the moment anything succeeds.
//
// "Running" cannot be asked of TESQuest::IsRunning alone. It shares a bit with
// IsEnabled and answers true for a quest that was enabled and never started, so
// a stale alias entry from a quest that finished hours ago would keep an NPC
// out of the pool for the rest of the save.
//
// And the kReserves flag is a rejection on its own, with no package and no
// scene: Skyrim's fill machinery treats a reserved hold as exclusive and would
// silently skip the actor during our own fill, leaving every alias empty and
// the dispatch dead with nothing to explain it.
//
// The alias array is built for real. ExtraAliasInstanceArray is a plain struct
// of arrays that the walk reads field by field, so a genuine one is both
// simpler than a stand-in and exactly faithful.

namespace
{
    namespace AliasWalkFilter = NarrativeEngine::AliasWalkFilter;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using Instance = EngineMock::AliasState::Instance;
    using QuestState = EngineMock::QuestState;

    constexpr std::uint32_t kActorFormID = 0x0001A6A0u;

    QuestState ForeignRunningQuest()
    {
        QuestState q;
        q.sourceFile = "SomeOtherMod.esp";
        q.editorID = "SomeOtherModQuest";
        q.running = true;
        q.stopped = false;
        q.completed = false;
        return q;
    }

    QuestState OurOwnQuest()
    {
        QuestState q = ForeignRunningQuest();
        q.sourceFile = "NarrativeEngine.esp";
        q.editorID = "NE_VisitQuest";
        return q;
    }

    // Fills the actor into whatever `engine.aliases.instances` describes.
    RE::Actor* ActorFilledIntoAliases(EngineMock& engine)
    {
        auto* actor = engine.AddActor(kActorFormID);
        engine.aliases.arrayPresent = true;
        engine.FillAliasInstances(actor);
        return actor;
    }

    bool StoryActive(RE::Actor* actor, std::string* reason = nullptr)
    {
        return AliasWalkFilter::IsActorStoryActive(actor, reason, /*debug=*/false);
    }
} // namespace

TEST_CASE("AliasWalkFilter::IsActorStoryActive with no alias data", "[AliasWalkFilter][engine]")
{
    EngineMock engine;

    SECTION("when there is no actor")
    {
        SECTION("should say not spoken for")
        {
            REQUIRE_FALSE(StoryActive(nullptr));
        }
    }

    SECTION("when the actor carries no alias array at all")
    {
        auto* actor = engine.AddActor(kActorFormID);
        engine.aliases.arrayPresent = false;

        SECTION("should say not spoken for")
        {
            // The common case for an ordinary NPC nobody has filled, and the
            // one the pool is mostly made of.
            REQUIRE_FALSE(StoryActive(actor));
        }
    }

    SECTION("when the actor is filled into nothing")
    {
        auto* actor = engine.AddActor(kActorFormID);
        engine.aliases.arrayPresent = true;
        engine.aliases.instances.clear();
        engine.FillAliasInstances(actor);

        SECTION("should say not spoken for")
        {
            REQUIRE_FALSE(StoryActive(actor));
        }
    }
}

TEST_CASE("AliasWalkFilter rejects an actor mid-scene", "[AliasWalkFilter][engine]")
{
    // The cheapest signal: whoever authored the scene is driving the actor
    // right now, and no alias walk is needed to know it.
    EngineMock engine;
    auto* actor = engine.AddActor(kActorFormID);
    engine.world.playerInScene = true;

    SECTION("when the scene belongs to another mod")
    {
        engine.aliases.sceneQuest = ForeignRunningQuest();

        SECTION("should say spoken for")
        {
            REQUIRE(StoryActive(actor));
        }

        SECTION("should name the scene in the reason")
        {
            // The reason reaches the candidate-pool census, which is the only
            // place a player can see why an NPC was never chosen.
            std::string reason;
            REQUIRE(StoryActive(actor, &reason));
            REQUIRE(reason.find("scene:") != std::string::npos);
        }
    }

    SECTION("when the scene is one of ours")
    {
        engine.aliases.sceneQuest = OurOwnQuest();

        SECTION("should keep walking rather than flag the actor")
        {
            // A sender mid-visit is in a scene our own visit quest owns.
            // Flagging them here would make every beat mark its own
            // participants unavailable the moment it succeeded.
            REQUIRE_FALSE(StoryActive(actor));
        }
    }
}

TEST_CASE("AliasWalkFilter walks the alias instances", "[AliasWalkFilter][engine]")
{
    // Happy path, re-run per leaf: an actor filled into one foreign, running
    // quest's alias that holds nothing against them. Each case changes one
    // property of that entry.
    EngineMock engine;

    SECTION("when a foreign running quest dispenses a package")
    {
        Instance instance;
        instance.quest = ForeignRunningQuest();
        instance.dispensesPackages = true;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should say spoken for")
        {
            // The engine's own record that the alias is driving this actor's
            // AI. Competing with it means two package stacks on one NPC.
            REQUIRE(StoryActive(actor));
        }

        SECTION("should name the quest and alias in the reason")
        {
            std::string reason;
            REQUIRE(StoryActive(actor, &reason));
            REQUIRE(reason.find("SomeOtherModQuest") != std::string::npos);
        }
    }

    SECTION("when a foreign running quest reserves the actor")
    {
        // No package and no scene: the flag alone is the rejection.
        Instance instance;
        instance.quest = ForeignRunningQuest();
        instance.reserves = true;
        instance.dispensesPackages = false;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should say spoken for")
        {
            // Skyrim's fill machinery treats a reserved hold as exclusive and
            // would silently skip this actor during our own fill, leaving the
            // dispatch dead with an unfilled alias and nothing to explain it.
            REQUIRE(StoryActive(actor));
        }

        SECTION("should say the actor was reserved")
        {
            std::string reason;
            REQUIRE(StoryActive(actor, &reason));
            REQUIRE(reason.find("reserved-by:") != std::string::npos);
        }
    }

    SECTION("when the alias holds nothing against the actor")
    {
        Instance instance;
        instance.quest = ForeignRunningQuest();
        instance.reserves = false;
        instance.dispensesPackages = false;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should say not spoken for")
        {
            // A tracking fill that neither reserves nor drives the actor is no
            // reason to exclude them; rejecting on the fill alone would empty
            // the pool of anyone any quest had ever noticed.
            REQUIRE_FALSE(StoryActive(actor));
        }
    }

    SECTION("when the quest is one of ours")
    {
        Instance instance;
        instance.quest = OurOwnQuest();
        instance.reserves = true;
        instance.dispensesPackages = true;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should skip it however it is flagged")
        {
            // Identified by ESP rather than by EditorID, so a beat added later
            // is self-excluded without anyone remembering to list it.
            REQUIRE_FALSE(StoryActive(actor));
        }
    }

    SECTION("when several aliases hold the actor")
    {
        // Only the last one is a real hold. A walk that stopped at the first
        // entry would miss it.
        Instance harmless;
        harmless.quest = ForeignRunningQuest();
        Instance ours;
        ours.quest = OurOwnQuest();
        ours.reserves = true;
        Instance real;
        real.quest = ForeignRunningQuest();
        real.reserves = true;
        engine.aliases.instances = {harmless, ours, real};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should keep walking until it finds one that matters")
        {
            REQUIRE(StoryActive(actor));
        }
    }
}

TEST_CASE("AliasWalkFilter ignores quests that are not running", "[AliasWalkFilter][engine]")
{
    // TESQuest::IsRunning shares a bit with IsEnabled and answers true for a
    // quest that was enabled and never started, so "running" is the conjunction
    // of three checks. A stale entry from a quest that finished hours ago would
    // otherwise keep an NPC out of the pool for the rest of the save.
    EngineMock engine;

    SECTION("when the quest has been stopped")
    {
        Instance instance;
        instance.quest = ForeignRunningQuest();
        instance.quest.stopped = true;
        instance.reserves = true;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should say not spoken for")
        {
            // Entries can linger past a Reset, which is exactly why the stopped
            // check exists rather than trusting the fill.
            REQUIRE_FALSE(StoryActive(actor));
        }
    }

    SECTION("when the quest has been completed")
    {
        Instance instance;
        instance.quest = ForeignRunningQuest();
        instance.quest.completed = true;
        instance.reserves = true;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should say not spoken for")
        {
            REQUIRE_FALSE(StoryActive(actor));
        }
    }

    SECTION("when the quest was never started")
    {
        Instance instance;
        instance.quest = ForeignRunningQuest();
        instance.quest.running = false;
        instance.reserves = true;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should say not spoken for")
        {
            REQUIRE_FALSE(StoryActive(actor));
        }
    }
}

TEST_CASE("AliasWalkFilter under trace logging", "[AliasWalkFilter][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=1\nbTraceMode=1\n"};

    SECTION("when every entry is logged")
    {
        Instance instance;
        instance.quest = ForeignRunningQuest();
        instance.reserves = true;
        engine.aliases.instances = {instance};
        auto* actor = ActorFilledIntoAliases(engine);

        SECTION("should reach the same verdict")
        {
            // The trace arm walks and decodes every entry rather than
            // short-circuiting, so it is the arm most able to drift. It must
            // not change the answer.
            REQUIRE(AliasWalkFilter::IsActorStoryActive(actor, nullptr, /*debug=*/true));
        }
    }
}
