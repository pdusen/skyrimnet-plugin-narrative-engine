#include <AmbushAttackerGroups.h>

#include <ConfiguredAttackerGroups.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Tests for who turns up when an ambush fires.
//
// The groups are content, not code: a file under the mod's data directory, full
// of editor IDs, that a player or another mod can edit and extend. So most of
// what can go wrong is a parse problem, and the module's whole posture towards
// those is to refuse the group rather than to guess. A group that silently
// half-loaded — its roster read but its "only at night" constraint dropped —
// would put a coven of vampires on the road at noon and nothing would say why.
//
// The eligibility rules read oddly until you see the reason: repeats of one key
// are OR, and different keys are AND. "Require two holds" means either of them,
// because a group that wanted both would be a group that could never fire. The
// exception is RequireGlobal, where repeats are AND, because each names a
// different variable and there is nothing for them to be alternatives to.
//
// One group being unconstrained is load-bearing rather than incidental. The
// beat needs the eligible set to be non-empty on any valid tick, and bandits
// being always-eligible is the whole of that guarantee.
//
// The hour window is the one piece of arithmetic here, and it wraps: 20-6 means
// evening through dawn. Implemented naively it inverts, and a nocturnal group
// becomes a daytime one that never appears when it should.

namespace
{
    namespace Groups = NarrativeEngine::AmbushAttackerGroups;
    using NarrativeEngine::Testing::ConfiguredAttackerGroups;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr std::uint32_t kPlayer = 0x00000014u;
    constexpr std::uint32_t kBanditList = 0x00E10001u;
    constexpr std::uint32_t kArcherList = 0x00E10002u;
    constexpr std::uint32_t kChiefList = 0x00E10003u;
    constexpr std::uint32_t kThievesGuild = 0x00E20001u;
    constexpr std::uint32_t kVampireKeyword = 0x00E30001u;
    constexpr std::uint32_t kWhiterunHold = 0x00016BE4u;
    constexpr std::uint32_t kFalkreathHold = 0x0001680Fu;
    constexpr std::uint32_t kBountyGlobal = 0x00E40001u;

    // The one global a case moves. Held on to because the parser resolves it
    // once at load: registering a second form under the same editor ID later
    // would leave the group pointing at the first.
    RE::TESGlobal* g_crimeGold = nullptr;

    // Everything a group file might name, registered so the parser can resolve
    // it. Registering more than a case needs costs nothing and keeps each case
    // about the key it is testing rather than about what was missing.
    void RegisterContent(EngineMock& engine)
    {
        (void)engine.AddLeveledCharacter(kBanditList, "LCharBandit");
        (void)engine.AddLeveledCharacter(kArcherList, "LCharBanditArcher");
        (void)engine.AddLeveledCharacter(kChiefList, "LCharBanditChief");
        (void)engine.AddFaction(kThievesGuild, "ThievesGuildFaction");
        (void)engine.AddKeyword("Vampire");
        (void)engine.AddKeyword("LocTypeHold");
        (void)engine.AddLocation(kWhiterunHold, "Whiterun Hold", {"LocTypeHold"}, "WhiterunHoldLocation");
        (void)engine.AddLocation(kFalkreathHold, "Falkreath Hold", {"LocTypeHold"}, "FalkreathHoldLocation");
        g_crimeGold = engine.AddGlobal(kBountyGlobal, "WhiterunCrimeGold", 0.0f);
    }

    // The smallest group that parses: an id and one rank-and-file list.
    const std::string kBanditsOnly = "[Group:bandits]\nLineForm = LCharBandit\n";

    bool HasGroup(const std::vector<const Groups::Group*>& groups, std::string_view id)
    {
        return std::any_of(groups.begin(), groups.end(), [&](const Groups::Group* g) { return g && g->id == id; });
    }

    Groups::EligibilityContext ContextFor(EngineMock& engine, std::uint32_t hold, float hour, std::uint16_t level)
    {
        Groups::EligibilityContext ctx;
        ctx.player = engine.AddActor(kPlayer);
        ctx.holdFormID = hold;
        ctx.gameHour = hour;
        ctx.nowGameHours = static_cast<double>(engine.calendar.hoursPassed);
        ctx.playerLevel = level;
        return ctx;
    }
} // namespace

TEST_CASE("AmbushAttackerGroups reads a group file", "[AmbushAttackerGroups][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    RegisterContent(engine);

    SECTION("when a group names a roster")
    {
        const ConfiguredAttackerGroups groups{"[Group:bandits]\n"
                                              "DisplayName = Bandits\n"
                                              "Flavor = a rough crew off the road\n"
                                              "LineForm = LCharBandit\n"
                                              "RangedForm = LCharBanditArcher\n"
                                              "LeaderForm = LCharBanditChief\n"};

        SECTION("should take the group")
        {
            REQUIRE(Groups::TotalGroupCount() == 1);
            REQUIRE(Groups::EnabledGroupCount() == 1);
        }

        SECTION("should resolve every form it named")
        {
            const auto* group = Groups::Find("bandits");
            REQUIRE(group != nullptr);
            REQUIRE(group->roster.lineForms.size() == 1);
            REQUIRE(group->roster.rangedForm != nullptr);
            REQUIRE(group->roster.leaderForm != nullptr);
        }

        SECTION("should keep what it is called")
        {
            const auto* group = Groups::Find("bandits");
            REQUIRE(group != nullptr);
            REQUIRE(group->displayName == "Bandits");
        }
    }

    SECTION("when a group has no rank and file")
    {
        const ConfiguredAttackerGroups groups{"[Group:hollow]\nDisplayName = Nobody\n"};

        SECTION("should refuse the group")
        {
            // An ambush with an empty roster spawns nothing and leaves the
            // beat waiting for attackers who never arrive.
            REQUIRE(Groups::TotalGroupCount() == 0);
        }
    }

    SECTION("when a group names a form that does not exist")
    {
        const ConfiguredAttackerGroups groups{"[Group:bandits]\nLineForm = LCharNothingAtAll\n"};

        SECTION("should refuse the group")
        {
            // Rather than dropping the one form and keeping the rest: a
            // roster missing a list is a group that spawns the wrong thing.
            REQUIRE(Groups::TotalGroupCount() == 0);
        }
    }

    SECTION("when a group is switched off")
    {
        const ConfiguredAttackerGroups groups{"[Group:bandits]\nLineForm = LCharBandit\nEnabled = false\n"};

        SECTION("should keep parsing it but never offer it")
        {
            // Parsed so that re-enabling it later cannot surprise anyone with
            // an error that was hiding all along.
            REQUIRE(Groups::TotalGroupCount() == 1);
            REQUIRE(Groups::EnabledGroupCount() == 0);
            REQUIRE(Groups::Find("bandits") == nullptr);
        }
    }

    SECTION("when a group's switch is a word rather than a boolean")
    {
        const ConfiguredAttackerGroups groups{"[Group:bandits]\nLineForm = LCharBandit\nEnabled = sometimes\n"};

        SECTION("should refuse the group rather than read it as off")
        {
            // A group vanishing because somebody typed a word the parser did
            // not know would be close to impossible to diagnose from outside.
            // ("yes" and "on" are among the ones it does know.)
            REQUIRE(Groups::TotalGroupCount() == 0);
        }
    }

    SECTION("when a group uses a key nothing knows")
    {
        const ConfiguredAttackerGroups groups{"[Group:bandits]\nLineForm = LCharBandit\nRequireMoonPhase = full\n"};

        SECTION("should keep the group and ignore the key")
        {
            // Unknown keys are how a group file written for a later version
            // is read by an earlier one, and refusing over them would break
            // every such file completely.
            REQUIRE(Groups::TotalGroupCount() == 1);
        }
    }

    SECTION("when there is no group file at all")
    {
        Groups::Load();

        SECTION("should end up with no groups")
        {
            // Loud in the log rather than silent: a beat that never fires
            // because its content is missing looks exactly like a beat that
            // is working and simply has not chosen to.
            REQUIRE(Groups::TotalGroupCount() == 0);
        }
    }
}

TEST_CASE("AmbushAttackerGroups decides which groups fit the moment", "[AmbushAttackerGroups][engine]")
{
    // Happy path, re-run per leaf: the player at level 10, in Whiterun Hold, at
    // noon, with an unconstrained group and one constrained group beside it.
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    RegisterContent(engine);
    const auto ctx = ContextFor(engine, kWhiterunHold, 12.0f, 10);

    SECTION("when a group asks for nothing")
    {
        const ConfiguredAttackerGroups groups{kBanditsOnly};

        SECTION("should always offer it")
        {
            // The one guarantee the beat has that its eligible set is never
            // empty on a valid tick.
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "bandits"));
        }
    }

    SECTION("when a group asks for a hold")
    {
        const ConfiguredAttackerGroups groups{
            kBanditsOnly + "[Group:locals]\nLineForm = LCharBandit\nRequireHold = WhiterunHoldLocation\n"};

        SECTION("should offer it inside that hold")
        {
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "locals"));
        }

        SECTION("should withhold it elsewhere")
        {
            const auto elsewhere = ContextFor(engine, kFalkreathHold, 12.0f, 10);
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(elsewhere), "locals"));
            REQUIRE(HasGroup(Groups::EligibleGroups(elsewhere), "bandits"));
        }

        SECTION("should withhold it where no hold resolved")
        {
            // Unclassified ground and mod-added worldspaces both land here,
            // and the unconstrained group carries the tick instead.
            const auto nowhere = ContextFor(engine, 0, 12.0f, 10);
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(nowhere), "locals"));
        }
    }

    SECTION("when a group names two holds")
    {
        const ConfiguredAttackerGroups groups{
            kBanditsOnly
            + "[Group:locals]\nLineForm = LCharBandit\n"
              "RequireHold = WhiterunHoldLocation\nRequireHold = FalkreathHoldLocation\n"};

        SECTION("should offer it in either")
        {
            // Repeats of one key are alternatives. Requiring both would be a
            // group that could never fire anywhere.
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "locals"));
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kFalkreathHold, 12.0f, 10)), "locals"));
        }
    }

    SECTION("when a group forbids a hold")
    {
        const ConfiguredAttackerGroups groups{
            kBanditsOnly + "[Group:outsiders]\nLineForm = LCharBandit\nForbidHold = WhiterunHoldLocation\n"};

        SECTION("should withhold it there and offer it elsewhere")
        {
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ctx), "outsiders"));
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kFalkreathHold, 12.0f, 10)), "outsiders"));
        }
    }

    SECTION("when a group asks about the player's allegiance")
    {
        const ConfiguredAttackerGroups groups{
            kBanditsOnly + "[Group:thieves]\nLineForm = LCharBandit\nRequirePlayerInFaction = ThievesGuildFaction\n"};

        SECTION("should withhold it from somebody unaffiliated")
        {
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ctx), "thieves"));
        }

        SECTION("should offer it to a member")
        {
            auto* player = engine.AddActor(kPlayer);
            auto* guild = engine.AddFaction(kThievesGuild, "ThievesGuildFaction");
            engine.SetFactionRank(player, guild, 1);
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "thieves"));
        }
    }

    SECTION("when a group asks about what the player has become")
    {
        const ConfiguredAttackerGroups groups{
            kBanditsOnly + "[Group:hunters]\nLineForm = LCharBandit\nRequirePlayerKeyword = Vampire\n"};

        SECTION("should withhold it from an ordinary player")
        {
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ctx), "hunters"));
        }

        SECTION("should offer it once they carry the keyword")
        {
            // Which is how vampirism is detected at all: turning swaps the
            // player's race for one carrying this keyword.
            auto* player = engine.AddActor(kPlayer);
            engine.GiveActorKeyword(player, engine.AddKeyword("Vampire"));
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "hunters"));
        }
    }

    SECTION("when a group is bounded by the player's level")
    {
        const ConfiguredAttackerGroups groups{
            kBanditsOnly + "[Group:veterans]\nLineForm = LCharBandit\nMinPlayerLevel = 20\nMaxPlayerLevel = 40\n"};

        SECTION("should withhold it below the floor")
        {
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ctx), "veterans"));
        }

        SECTION("should offer it inside the band")
        {
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 12.0f, 25)), "veterans"));
        }

        SECTION("should withhold it above the ceiling")
        {
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 12.0f, 60)), "veterans"));
        }
    }

    SECTION("when the level bounds are the wrong way round")
    {
        const ConfiguredAttackerGroups groups{"[Group:impossible]\nLineForm = LCharBandit\n"
                                              "MinPlayerLevel = 40\nMaxPlayerLevel = 20\n"};

        SECTION("should refuse the group")
        {
            // A group that can never be eligible is a mistake in the file
            // rather than an intention, and refusing it says so.
            REQUIRE(Groups::TotalGroupCount() == 0);
        }
    }

    SECTION("when a group asks about a global")
    {
        const ConfiguredAttackerGroups groups{
            kBanditsOnly + "[Group:wanted]\nLineForm = LCharBandit\nRequireGlobal = WhiterunCrimeGold > 100\n"};

        SECTION("should withhold it while the global is low")
        {
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ctx), "wanted"));
        }

        SECTION("should offer it once the global rises")
        {
            // Read live rather than at load: these are quest-script-written
            // world state, and a value cached at startup would be the value
            // from the previous session.
            g_crimeGold->value = 500.0f;
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "wanted"));
        }
    }
}

TEST_CASE("AmbushAttackerGroups keeps a nocturnal group to the night", "[AmbushAttackerGroups][engine]")
{
    // The window wraps across midnight, which is the case that matters and the
    // one a naive implementation inverts.
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    RegisterContent(engine);

    SECTION("when the window runs from evening to dawn")
    {
        const ConfiguredAttackerGroups groups{kBanditsOnly
                                              + "[Group:nightfolk]\nLineForm = LCharBandit\nRequireGameHour = 20-6\n"};

        SECTION("should offer it late in the evening")
        {
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 22.0f, 10)), "nightfolk"));
        }

        SECTION("should offer it in the small hours")
        {
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 3.0f, 10)), "nightfolk"));
        }

        SECTION("should withhold it at noon")
        {
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 12.0f, 10)), "nightfolk"));
        }

        SECTION("should include the hour it opens and exclude the one it closes")
        {
            // Half-open, so two adjoining windows cannot both claim the same
            // hour.
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 20.0f, 10)), "nightfolk"));
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 6.0f, 10)), "nightfolk"));
        }
    }

    SECTION("when the window runs through the day")
    {
        const ConfiguredAttackerGroups groups{kBanditsOnly
                                              + "[Group:daylight]\nLineForm = LCharBandit\nRequireGameHour = 8-18\n"};

        SECTION("should offer it inside and withhold it outside")
        {
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 12.0f, 10)), "daylight"));
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 2.0f, 10)), "daylight"));
        }
    }

    SECTION("when the window's two bounds are the same hour")
    {
        const ConfiguredAttackerGroups groups{"[Group:ambiguous]\nLineForm = LCharBandit\nRequireGameHour = 6-6\n"};

        SECTION("should refuse the group")
        {
            // It could mean never or always, and both readings are defensible
            // — which is exactly why it must not be guessed at.
            REQUIRE(Groups::TotalGroupCount() == 0);
        }
    }
}

TEST_CASE("AmbushAttackerGroups sits a group out after it is used", "[AmbushAttackerGroups][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    RegisterContent(engine);
    engine.calendar.hoursPassed = 1000.0f;
    // Three groups: one with no cooldown at all, one a case uses, and one
    // nothing ever touches. The stamps are session state and outlive a reload
    // of the file — they go to the co-save in the shipping build — so a case
    // about a group that has never been used needs one no other case stamped.
    const ConfiguredAttackerGroups groups{"[Group:bandits]\nLineForm = LCharBandit\nCooldownGameHours = 0\n"
                                          "[Group:raiders]\nLineForm = LCharBandit\nCooldownGameHours = 24\n"
                                          "[Group:strangers]\nLineForm = LCharBandit\nCooldownGameHours = 24\n"};
    const auto ctx = ContextFor(engine, kWhiterunHold, 12.0f, 10);

    SECTION("when a group has just been used")
    {
        Groups::StampGroupUsed("raiders");

        SECTION("should withhold it")
        {
            // Not stamped when a group is merely considered — a compose that
            // failed would otherwise retire the group for a day over nothing.
            REQUIRE_FALSE(HasGroup(Groups::EligibleGroups(ctx), "raiders"));
        }

        SECTION("should say how long it has left")
        {
            REQUIRE(Groups::RemainingCooldownGameHours("raiders") > 0.0);
        }

        SECTION("should leave the others alone")
        {
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "strangers"));
        }
    }

    SECTION("when the cooldown has run out")
    {
        Groups::StampGroupUsed("raiders");
        engine.calendar.hoursPassed = 1000.0f + 25.0f;

        SECTION("should offer it again")
        {
            REQUIRE(HasGroup(Groups::EligibleGroups(ContextFor(engine, kWhiterunHold, 12.0f, 10)), "raiders"));
        }
    }

    SECTION("when a group has never been used")
    {
        SECTION("should have no cooldown to serve")
        {
            REQUIRE(Groups::RemainingCooldownGameHours("strangers") == 0.0);
        }
    }

    SECTION("when a group is configured to have no cooldown")
    {
        Groups::StampGroupUsed("bandits");

        SECTION("should be offered again at once")
        {
            // Zero means off. A group firing twice running is a choice a
            // group file is allowed to make.
            REQUIRE(HasGroup(Groups::EligibleGroups(ctx), "bandits"));
            REQUIRE(Groups::RemainingCooldownGameHours("bandits") == 0.0);
        }
    }
}

TEST_CASE("AmbushAttackerGroups builds a roster for a count", "[AmbushAttackerGroups][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    RegisterContent(engine);
    const ConfiguredAttackerGroups groups{"[Group:bandits]\n"
                                          "LineForm = LCharBandit\n"
                                          "RangedForm = LCharBanditArcher\n"
                                          "LeaderForm = LCharBanditChief\n"};
    const auto* group = Groups::Find("bandits");

    SECTION("when a full party is asked for")
    {
        REQUIRE(group != nullptr);
        const auto roster = Groups::ComposeRoster(*group, 6);

        SECTION("should be exactly as big as asked")
        {
            REQUIRE(roster.size() == 6);
        }

        SECTION("should put one leader at the head of it")
        {
            REQUIRE(std::count(roster.begin(), roster.end(), group->roster.leaderForm) == 1);
        }

        SECTION("should mix in about one archer in three")
        {
            REQUIRE(std::count(roster.begin(), roster.end(), group->roster.rangedForm) == 2);
        }
    }

    SECTION("when only a pair is asked for")
    {
        REQUIRE(group != nullptr);
        const auto roster = Groups::ComposeRoster(*group, 2);

        SECTION("should leave the leader out")
        {
            // A leader among two attackers does not read as a leader, it
            // reads as one of the two being oddly dressed.
            REQUIRE(std::count(roster.begin(), roster.end(), group->roster.leaderForm) == 0);
            REQUIRE(roster.size() == 2);
        }
    }

    SECTION("when nobody is asked for")
    {
        REQUIRE(group != nullptr);

        SECTION("should build nothing")
        {
            REQUIRE(Groups::ComposeRoster(*group, 0).empty());
        }
    }
}
