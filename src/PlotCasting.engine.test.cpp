#include <PlotCasting.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Tests for who a plot step may be given to.
//
// The ladder decides who may be asked; a decision chooses among them. What
// is pinned here is the asking half: that the best rung with anybody on it is
// all that is offered, that the mastermind joins only where the ladder lets
// them, and that a plot may send the same person twice in a row while
// another plot still may not borrow them.

namespace
{
    namespace PlotCasting = NarrativeEngine::PlotCasting;
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace PlotRecipes = NarrativeEngine::PlotRecipes;

    constexpr RE::FormID kFaction = 0x0000C001;
    constexpr RE::FormID kBoss = 0x00010001;
    constexpr RE::FormID kTiedSubordinate = 0x00010002; // rung 1
    constexpr RE::FormID kSubordinate = 0x00010003;     // rung 2
    constexpr RE::FormID kAcquaintance = 0x00010004;    // rung 3
    constexpr RE::FormID kStranger = 0x00010005;        // no rung

    constexpr double kToday = 10.0;

    PlotCasting::Member Person(RE::FormID npc, const char* name)
    {
        PlotCasting::Member m;
        m.npc = npc;
        m.name = name;
        return m;
    }

    PlotCasting::Population Court()
    {
        PlotCasting::Population p;
        auto boss = Person(kBoss, "Olfrid");
        boss.factions.push_back({kFaction, 1.0, true});
        boss.ties.push_back({kTiedSubordinate, true, 0});
        boss.ties.push_back({kAcquaintance, false, 0});
        p.members.push_back(boss);

        auto tied = Person(kTiedSubordinate, "Idolaf");
        tied.factions.push_back({kFaction, 0.5, true});
        p.members.push_back(tied);

        auto subordinate = Person(kSubordinate, "Jon");
        subordinate.factions.push_back({kFaction, 0.2, true});
        p.members.push_back(subordinate);

        p.members.push_back(Person(kAcquaintance, "Hulda"));
        p.members.push_back(Person(kStranger, "Brand-Shei"));
        return p;
    }

    struct Fixture
    {
        PlotCasting::Population population = Court();
        PlotCasting::OccupancyTable occupancy;
        std::unordered_set<RE::FormID> exempt;
        std::string stepText = "Someone watches someone, learning their habits.";

        PlotCasting::Admit admit;

        PlotCasting::Result Cast()
        {
            return PlotCasting::EligibleAgents(
                population, kBoss, stepText, occupancy, kToday, {}, 0, {}, exempt, admit);
        }

        std::vector<RE::FormID> Offered()
        {
            return Cast().shortlist;
        }

        // `npc` practises the craft, at this skill out of 100.
        void Crafts(RE::FormID npc, PlotRecipes::Craft craft, double skill)
        {
            for (auto& m : population.members) {
                if (m.npc == npc) {
                    m.crafts |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(craft));
                    m.skills.smithing = skill / 100.0;
                    m.skills.alchemy = skill / 100.0;
                    m.skills.enchanting = skill / 100.0;
                }
            }
        }

        // Only those able to make this.
        void Making(const PlotRecipes::Recipe& recipe)
        {
            admit = [recipe](const PlotCasting::Member& m) { return PlotCasting::CanMake(m, recipe); };
        }

        void Busy(RE::FormID npc)
        {
            occupancy[npc].plotId = 99;
            occupancy[npc].role = PlotModel::Role::Actor;
        }

        void Resting(RE::FormID npc)
        {
            occupancy[npc].plotId = 0;
            occupancy[npc].actorAvailableAtGameDay = kToday + 1.0;
        }
    };
} // namespace

TEST_CASE("PlotCasting::EligibleAgents", "[PlotCasting][engine]")
{
    Fixture f;

    SECTION("when a tied subordinate is free")
    {
        SECTION("should offer that rung alone, without the mastermind")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kTiedSubordinate});
        }
    }

    SECTION("when the first rung is busy")
    {
        f.Busy(kTiedSubordinate);

        SECTION("should fall to the next rung")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kSubordinate});
        }
    }

    SECTION("when only an acquaintance is left")
    {
        f.Busy(kTiedSubordinate);
        f.Busy(kSubordinate);

        SECTION("should offer them and the mastermind, who may as well do it")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kAcquaintance, kBoss});
        }
    }

    SECTION("when nobody on any rung is free")
    {
        f.Busy(kTiedSubordinate);
        f.Busy(kSubordinate);
        f.Busy(kAcquaintance);

        SECTION("should leave the mastermind to do it themselves")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kBoss});
        }
    }

    SECTION("when the plot's own last agent is resting")
    {
        f.Resting(kTiedSubordinate);

        SECTION("should not offer them to a step that is not theirs to repeat")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kSubordinate});
        }

        SECTION("should offer them again to their own plot")
        {
            f.exempt.insert(kTiedSubordinate);
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kTiedSubordinate});
        }
    }

    SECTION("when the plot's own last agent is busy in another plot")
    {
        f.Busy(kTiedSubordinate);
        f.exempt.insert(kTiedSubordinate);

        SECTION("should not offer them; the exemption is from rest, not from work")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kSubordinate});
        }
    }

    SECTION("when the step's own sentence names a would-be agent")
    {
        f.stepText = "Someone watches Idolaf, learning their habits.";

        SECTION("should not send them to do it to themselves")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kSubordinate});
        }
    }

    SECTION("when anyone is offered")
    {
        SECTION("should never offer a stranger")
        {
            f.Busy(kTiedSubordinate);
            f.Busy(kSubordinate);
            const auto offered = f.Offered();
            REQUIRE(std::find(offered.begin(), offered.end(), kStranger) == offered.end());
        }
    }
}

TEST_CASE("PlotCasting crafting agents", "[PlotCasting][engine]")
{
    Fixture f;
    PlotRecipes::Recipe sword;
    sword.form = 0x0000D001;
    sword.displayName = "a Steel Sword";
    sword.craft = PlotRecipes::Craft::Smith;
    sword.skill = 30;
    f.Making(sword);

    SECTION("when a subordinate further down the ladder is the only smith")
    {
        f.Crafts(kSubordinate, PlotRecipes::Craft::Smith, 40);

        SECTION("should pass over the rung above, which has nobody who can make it")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kSubordinate});
        }
    }

    SECTION("when a smith on the ladder lacks the skill the recipe asks")
    {
        f.Crafts(kTiedSubordinate, PlotRecipes::Craft::Smith, 20);
        f.Crafts(kSubordinate, PlotRecipes::Craft::Smith, 30);

        SECTION("should offer only the one skilled enough")
        {
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kSubordinate});
        }
    }

    SECTION("when somebody practises another craft")
    {
        f.Crafts(kTiedSubordinate, PlotRecipes::Craft::Concoct, 100);

        SECTION("should not send them to the forge")
        {
            REQUIRE(f.Offered().empty());
        }
    }

    SECTION("when nobody on the ladder can make it")
    {
        f.Crafts(kStranger, PlotRecipes::Craft::Smith, 100);

        SECTION("should leave the mastermind out unless they can make it themselves")
        {
            REQUIRE(f.Offered().empty());
            f.Crafts(kBoss, PlotRecipes::Craft::Smith, 50);
            REQUIRE(f.Offered() == std::vector<RE::FormID>{kBoss});
        }
    }

    SECTION("when the only smith is busy")
    {
        f.Crafts(kSubordinate, PlotRecipes::Craft::Smith, 40);
        f.Busy(kSubordinate);
        const auto cast = f.Cast();

        SECTION("should offer nobody, and say the smith was busy rather than absent")
        {
            REQUIRE(cast.shortlist.empty());
            REQUIRE(cast.considered.size() == 1);
            REQUIRE(cast.considered[0].npc == kSubordinate);
            REQUIRE(cast.considered[0].reject == PlotCasting::Reject::Occupied);
        }
    }
}

TEST_CASE("PlotCasting keeps the people a step names from carrying it out", "[PlotCasting][engine]")
{
    // Every one of them tied to the mastermind and below them in their
    // faction, so each is a tied subordinate and only the step's words can
    // keep them off the ladder.
    constexpr RE::FormID kVeleth = 0x00030001;
    constexpr RE::FormID kOthreloth = 0x00030002;
    constexpr RE::FormID kAsgeir = 0x00030003;
    constexpr RE::FormID kRyan = 0x00030004;
    Fixture f;
    f.population.members.clear();
    auto boss = Person(kBoss, "Olfrid");
    for (const auto npc : {kVeleth, kOthreloth, kAsgeir, kRyan}) {
        boss.ties.push_back({npc, true, 0});
    }
    boss.factions.push_back({kFaction, 1.0, true});
    f.population.members.push_back(boss);
    for (const auto& [npc, name] : {std::pair{kVeleth, "Captain Veleth"},
                                    std::pair{kOthreloth, "Elder Othreloth"},
                                    std::pair{kAsgeir, "Asgeir Snow-Shod"},
                                    std::pair{kRyan, "Ryan Lod"}}) {
        auto member = Person(npc, name);
        member.factions.push_back({kFaction, 0.5, true});
        f.population.members.push_back(member);
    }
    // Whether `npc` may carry out the step `text` describes.
    const auto may = [&f](RE::FormID npc, const char* text) {
        return PlotCasting::QualifyAgent(f.population, kBoss, npc, text, 0, f.occupancy, kToday, {}, {}).Eligible();
    };
    // Each of the four may carry out a step that names none of them.
    REQUIRE(may(kVeleth, "Watch the market"));
    REQUIRE(may(kOthreloth, "Watch the market"));
    REQUIRE(may(kAsgeir, "Watch the market"));
    REQUIRE(may(kRyan, "Watch the market"));

    SECTION("a name, or a word of it, is the person, possessive or in any case")
    {
        CHECK_FALSE(may(kVeleth, "Watch Captain Veleth's movements"));
        CHECK_FALSE(may(kAsgeir, "Watch Asgeir and Vittoria at the Bee and Barb"));
        CHECK_FALSE(may(kAsgeir, "Search the Snow-Shod's farm"));
        CHECK_FALSE(may(kAsgeir, "watch asgeir"));
        CHECK_FALSE(may(kRyan, "Watch Ryan"));
    }

    SECTION("a title alone names nobody")
    {
        CHECK(may(kOthreloth, "Ask the elder, then the captain"));
        CHECK(may(kVeleth, "Ask the elder, then the captain"));
    }

    SECTION("a name inside a longer word, or a short word of a name, does not")
    {
        CHECK(may(kVeleth, "Watch Velethia"));
        CHECK(may(kAsgeir, "Find an Asgeir-like groom"));
        CHECK(may(kRyan, "Watch O'Ryan"));
        CHECK(may(kRyan, "Watch Lod"));
    }
}
