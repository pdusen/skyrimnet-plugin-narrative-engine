#include <PlotCasting.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
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

        std::vector<RE::FormID> Offered()
        {
            return PlotCasting::EligibleAgents(population, kBoss, stepText, occupancy, kToday, {}, 0, {}, exempt)
                .shortlist;
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
