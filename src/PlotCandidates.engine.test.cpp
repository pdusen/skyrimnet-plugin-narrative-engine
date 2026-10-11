#include <PlotCandidates.h>

#include <EngineMock.h>
#include <PlotPopulation.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

// Tests for the candidate rules: what each verb slot may be filled with.
//
// The province here is fabricated as plain population records, because the
// rules read nothing else. Who is on each list, and in what order, is the
// whole contract: an admitted stranger reads as a scheme reaching for
// somebody it has no reason to know, and an unstable order would renumber a
// list between the question and the answer.

namespace
{
    namespace PlotCandidates = NarrativeEngine::PlotCandidates;
    namespace PlotCasting = NarrativeEngine::PlotCasting;
    namespace PlotMenus = NarrativeEngine::PlotMenus;
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace PlotVerbs = NarrativeEngine::PlotVerbs;
    using NarrativeEngine::Testing::EngineMock;

    constexpr RE::FormID kWhiterun = 0x0000A001;
    constexpr RE::FormID kRiverwood = 0x0000A002;
    constexpr RE::FormID kRiften = 0x0000A003;
    constexpr RE::FormID kWhiterunHold = 0x0000B001;
    constexpr RE::FormID kRiftHold = 0x0000B002;
    constexpr RE::FormID kCompanions = 0x0000C001;

    // The mastermind and, around them, one person of each kind a rule cares
    // about.
    constexpr RE::FormID kBoss = 0x00010001;      // Olfrid, Whiterun
    constexpr RE::FormID kFriend = 0x00010002;    // a tie, in Whiterun
    constexpr RE::FormID kColleague = 0x00010003; // shares the Companions, standing 0.6
    constexpr RE::FormID kNeighbour = 0x00010004; // same hold, nothing else; a farmer
    constexpr RE::FormID kEnemy = 0x00010005;     // a hostile tie
    constexpr RE::FormID kStranger = 0x00010006;  // Riften, a merchant
    constexpr RE::FormID kJarl = 0x00010007;      // Riften, an authority
    constexpr RE::FormID kDead = 0x00010008;      // a tie, but dead

    PlotCasting::Member Person(RE::FormID npc, std::string name, RE::FormID hold, RE::FormID settlement)
    {
        PlotCasting::Member m;
        m.npc = npc;
        m.name = std::move(name);
        m.hold = hold;
        m.settlement = settlement;
        return m;
    }

    PlotCasting::Population Province()
    {
        PlotCasting::Population p;
        auto boss = Person(kBoss, "Olfrid", kWhiterunHold, kWhiterun);
        boss.factions.push_back({kCompanions, 1.0, true});
        boss.ties.push_back({kFriend, false, 0});
        boss.ties.push_back({kEnemy, false, +1});
        boss.ties.push_back({kDead, false, 0});
        p.members.push_back(boss);

        auto friendly = Person(kFriend, "Idolaf", kWhiterunHold, kWhiterun);
        friendly.ties.push_back({kStranger, false, 0}); // the friend knows somebody in Riften
        p.members.push_back(friendly);

        auto colleague = Person(kColleague, "Vilkas", kWhiterunHold, kWhiterun);
        colleague.factions.push_back({kCompanions, 0.6, true});
        p.members.push_back(colleague);

        auto neighbour = Person(kNeighbour, "Hulda", kWhiterunHold, kRiverwood);
        neighbour.tradesForLiving = true;
        p.members.push_back(neighbour);

        p.members.push_back(Person(kEnemy, "Vignar", kWhiterunHold, kWhiterun));

        auto stranger = Person(kStranger, "Brand-Shei", kRiftHold, kRiften);
        stranger.tradesForLiving = true;
        p.members.push_back(stranger);

        auto jarl = Person(kJarl, "Laila Law-Giver", kRiftHold, kRiften);
        jarl.authority = true;
        p.members.push_back(jarl);

        p.members.push_back(Person(kDead, "Ghost of Somebody", kWhiterunHold, kWhiterun));
        return p;
    }

    PlotMenus::World World()
    {
        PlotMenus::World w;
        w.settlements = {{kWhiterun, "Whiterun"}, {kRiverwood, "Riverwood"}, {kRiften, "Riften"}};
        w.items = {{0x0000D001, "a ruby", {}}, {0x0000D002, "a bottle of skooma", {}}};
        return w;
    }

    // Everything one call needs, owned in one place so a section can adjust
    // what it cares about and leave the rest alone.
    struct Fixture
    {
        PlotCasting::Population population = Province();
        PlotMenus::World world = World();
        std::vector<PlotModel::SlotValue> filled;
        std::vector<PlotModel::Step> steps;
        std::vector<const PlotModel::Step*> earlier;

        PlotCandidates::Request Request()
        {
            earlier.clear();
            for (const auto& s : steps) {
                earlier.push_back(&s);
            }
            PlotCandidates::Request r{population, kBoss, world, filled, earlier, {}, {}};
            r.alive = [](RE::FormID npc) { return npc != kDead; };
            return r;
        }
    };

    PlotVerbs::Slot SlotOf(std::string_view verbId, std::string_view name)
    {
        const auto* verb = PlotVerbs::Find(verbId);
        REQUIRE(verb != nullptr);
        const auto* slot = PlotVerbs::FindSlot(*verb, name);
        REQUIRE(slot != nullptr);
        return *slot;
    }

    std::vector<RE::FormID> Forms(const std::vector<PlotCandidates::Candidate>& list)
    {
        std::vector<RE::FormID> out;
        for (const auto& c : list) {
            out.push_back(c.value.form);
        }
        return out;
    }

    bool Has(const std::vector<PlotCandidates::Candidate>& list, RE::FormID form)
    {
        const auto forms = Forms(list);
        return std::find(forms.begin(), forms.end(), form) != forms.end();
    }
} // namespace

TEST_CASE("PlotCandidates people rules", "[PlotCandidates][engine]")
{
    Fixture f;

    SECTION("when anybody known to the mastermind is asked for")
    {
        const auto list = PlotCandidates::Build(SlotOf("watch", "mark"), f.Request());

        SECTION("should admit a tie, a colleague and a neighbour, most connected first")
        {
            // Idolaf and Vignar are both plain ties; FormID breaks the tie.
            REQUIRE(Forms(list) == std::vector<RE::FormID>{kFriend, kEnemy, kColleague, kNeighbour});
        }

        SECTION("should refuse a stranger, the dead and the mastermind")
        {
            REQUIRE_FALSE(Has(list, kStranger));
            REQUIRE_FALSE(Has(list, kDead));
            REQUIRE_FALSE(Has(list, kBoss));
        }

        SECTION("should key the list c1 onward and fill the slot it was asked for")
        {
            REQUIRE(list.front().key == "c1");
            REQUIRE(list.back().key == "c4");
            REQUIRE(list.front().value.name == "mark");
            REQUIRE(list.front().value.text == "Idolaf");
        }

        SECTION("should say on each line why the person is there")
        {
            REQUIRE(list.front().line == "Idolaf (someone they know)");
        }
    }

    SECTION("when a friend is asked for")
    {
        const auto list = PlotCandidates::Build(SlotOf("win_over", "mark"), f.Request());

        SECTION("should refuse somebody hostile to the mastermind")
        {
            REQUIRE_FALSE(Has(list, kEnemy));
            REQUIRE(Has(list, kFriend));
        }
    }

    SECTION("when somebody who trades for a living is asked for")
    {
        const auto list = PlotCandidates::Build(SlotOf("buy", "seller"), f.Request());

        SECTION("should admit only traders, a known one before a stranger")
        {
            REQUIRE(Forms(list) == std::vector<RE::FormID>{kNeighbour, kStranger});
        }

        SECTION("should place a stranger by where they live")
        {
            REQUIRE(list.back().line == "Brand-Shei (lives in Riften)");
        }
    }

    SECTION("when an authority is asked for")
    {
        SECTION("should admit only a jarl, steward, captain or chief")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("denounce", "authority"), f.Request()))
                    == std::vector<RE::FormID>{kJarl});
        }
    }

    SECTION("when somebody with standing is asked for")
    {
        SECTION("should admit only those with standing in a rostered faction")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("bribe", "mark"), f.Request()))
                    == std::vector<RE::FormID>{kColleague});
        }
    }

    SECTION("when another slot of the same step is already filled")
    {
        f.filled.push_back({"mark", kFriend, "Idolaf"});

        SECTION("should not offer the same person twice")
        {
            REQUIRE_FALSE(Has(PlotCandidates::Build(SlotOf("denounce", "mark"), f.Request()), kFriend));
        }
    }

    SECTION("when an occupation is known")
    {
        auto request = f.Request();
        request.occupation = [](RE::FormID npc) {
            return npc == kNeighbour ? std::string{"runs the Bannered Mare"} : std::string{};
        };
        const auto list = PlotCandidates::Build(SlotOf("buy", "seller"), request);

        SECTION("should put it on the line, and leave it off for nobody else")
        {
            REQUIRE(list.front().line == "Hulda, runs the Bannered Mare (lives in the same hold)");
            REQUIRE(list.back().line == "Brand-Shei (lives in Riften)");
        }
    }

    SECTION("when the scheme names a stranger")
    {
        auto request = f.Request();
        request.scheme = "steal Brand-Shei's ledger";
        const auto list = PlotCandidates::Build(SlotOf("watch", "mark"), request);

        SECTION("should admit them to a slot about whom the mastermind knows, first, saying why")
        {
            REQUIRE(list.front().value.form == kStranger);
            REQUIRE(list.front().line == "Brand-Shei (lives in Riften; named in the scheme)");
            REQUIRE(list.size() == 5);
        }
    }

    SECTION("when the scheme names somebody the rule cannot admit")
    {
        auto request = f.Request();
        request.scheme = "ruin Vilkas";

        SECTION("should still keep a non-trader off a trader's list")
        {
            REQUIRE_FALSE(Has(PlotCandidates::Build(SlotOf("buy", "seller"), request), kColleague));
        }
    }

    SECTION("when the scheme names somebody a property rule admits")
    {
        auto request = f.Request();
        request.scheme = "buy out Brand-Shei";

        SECTION("should put them first even though somebody known also qualifies")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("buy", "seller"), request))
                    == std::vector<RE::FormID>{kStranger, kNeighbour});
        }
    }

    SECTION("when the same request is made twice")
    {
        SECTION("should answer in the same order both times")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("plant", "mark"), f.Request()))
                    == Forms(PlotCandidates::Build(SlotOf("plant", "mark"), f.Request())));
        }
    }

    SECTION("when more people qualify than the limit")
    {
        PlotCandidates::Limits limits;
        limits.people = 2;

        SECTION("should keep the most connected")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("watch", "mark"), f.Request(), limits))
                    == std::vector<RE::FormID>{kFriend, kEnemy});
        }
    }
}

TEST_CASE("PlotCandidates::NamedIn", "[PlotCandidates][engine]")
{
    PlotCasting::Population p;
    p.members.push_back(Person(0x00020001, "Maven Black-Briar", kRiftHold, kRiften));
    p.members.push_back(Person(0x00020002, "Hemming Black-Briar", kRiftHold, kRiften));
    p.members.push_back(Person(0x00020003, "Ingun Black-Briar", kRiftHold, kRiften));
    p.members.push_back(Person(0x00020004, "Elder Othreloth", kRiftHold, kRiften));
    p.members.push_back(Person(0x00020005, "Ri'saad", kRiftHold, kRiften));

    SECTION("when the scheme gives a first name")
    {
        SECTION("should name that person and nobody else in the family")
        {
            REQUIRE(PlotCandidates::NamedIn(p, "steal three flawless amethysts from Maven Black-Briar")
                    == std::unordered_set<RE::FormID>{0x00020001});
        }
    }

    SECTION("when the scheme gives only a surname three people share")
    {
        SECTION("should name nobody")
        {
            REQUIRE(PlotCandidates::NamedIn(p, "ruin the Black-Briar family").empty());
        }
    }

    SECTION("when the scheme gives part of a name, possessive, or with an apostrophe inside")
    {
        SECTION("should find them")
        {
            REQUIRE(PlotCandidates::NamedIn(p, "forge a ledger showing Othreloth's theft").contains(0x00020004));
            REQUIRE(PlotCandidates::NamedIn(p, "rob Ri'saad's caravan").contains(0x00020005));
        }
    }

    SECTION("when a name appears only inside another word, or in lowercase")
    {
        SECTION("should name nobody")
        {
            REQUIRE(PlotCandidates::NamedIn(p, "steal Mavenhold's charter").empty());
            REQUIRE(PlotCandidates::NamedIn(p, "find maven's ledger").empty());
        }
    }
}

TEST_CASE("PlotCandidates places", "[PlotCandidates][engine]")
{
    Fixture f;

    SECTION("when a place is asked for before its mark is chosen")
    {
        SECTION("should offer nothing, since the place depends on the mark")
        {
            REQUIRE(PlotCandidates::Build(SlotOf("ask_around", "place"), f.Request()).empty());
        }
    }

    SECTION("when asking around about a mark")
    {
        f.filled.push_back({"mark", kFriend, "Idolaf"});
        const auto list = PlotCandidates::Build(SlotOf("ask_around", "place"), f.Request());

        SECTION("should offer where the mark lives, then where those close to them live")
        {
            REQUIRE(Forms(list) == std::vector<RE::FormID>{kWhiterun, kRiften});
            REQUIRE(list.front().value.text == "Whiterun");
        }
    }

    SECTION("when spreading talk about a mark")
    {
        f.filled.push_back({"mark", kStranger, "Brand-Shei"});

        SECTION("should offer the mark's settlement and the mastermind's")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("spread_rumors", "place"), f.Request()))
                    == std::vector<RE::FormID>{kRiften, kWhiterun});
        }
    }
}

TEST_CASE("PlotCandidates things and bindings", "[PlotCandidates][engine]")
{
    Fixture f;

    SECTION("when an item is chosen from the pool")
    {
        const auto list = PlotCandidates::Build(SlotOf("steal", "item"), f.Request());

        SECTION("should offer the pool in its own order, as it is said aloud")
        {
            REQUIRE(Forms(list) == std::vector<RE::FormID>{0x0000D001, 0x0000D002});
            REQUIRE(list.front().line == "a ruby");
        }

        SECTION("should be asked")
        {
            REQUIRE(PlotCandidates::Asks(SlotOf("steal", "item")));
        }
    }

    SECTION("when the pool holds more items than a person list would")
    {
        for (RE::FormID i = 0; i < 30; ++i) {
            f.world.items.push_back({0x0000E000 + i, "a trinket", {}});
        }

        SECTION("should offer every one of them")
        {
            REQUIRE(PlotCandidates::Build(SlotOf("steal", "item"), f.Request()).size() == 32);
        }
    }

    SECTION("when an item is bound to an earlier step")
    {
        PlotModel::Step steal;
        steal.verb = "steal";
        steal.outcome = PlotModel::StepOutcome::Succeeded;
        steal.slots.push_back({"item", 0x0000D002, "a bottle of skooma"});
        PlotModel::Step watch;
        watch.verb = "watch";
        f.steps = {steal, watch};

        SECTION("should ask nothing")
        {
            REQUIRE_FALSE(PlotCandidates::Asks(SlotOf("deliver", "item")));
        }

        SECTION("should be the item the latest obtaining step got")
        {
            const auto bound = PlotCandidates::Bind(SlotOf("deliver", "item"), f.Request());
            REQUIRE(bound.has_value());
            REQUIRE(bound->form == 0x0000D002);
            REQUIRE(bound->name == "item");
        }
    }

    SECTION("when the step that would have got the item failed, or has not happened")
    {
        PlotModel::Step caught;
        caught.verb = "steal";
        caught.outcome = PlotModel::StepOutcome::FailedCaught;
        caught.slots.push_back({"item", 0x0000D002, "a bottle of skooma"});
        PlotModel::Step planned;
        planned.verb = "buy";
        planned.slots.push_back({"item", 0x0000D001, "a ruby"});
        f.steps = {caught, planned};

        SECTION("should bind to nothing, since nobody has it")
        {
            REQUIRE_FALSE(PlotCandidates::Bind(SlotOf("deliver", "item"), f.Request()).has_value());
        }
    }

    SECTION("when a person is excluded")
    {
        auto request = f.Request();
        request.excluded = {kFriend};

        SECTION("should keep them off every list")
        {
            REQUIRE_FALSE(Has(PlotCandidates::Build(SlotOf("watch", "mark"), request), kFriend));
        }
    }

    SECTION("when nothing earlier obtained an item")
    {
        SECTION("should bind to nothing, so the verb is not offered")
        {
            REQUIRE_FALSE(PlotCandidates::Bind(SlotOf("plant", "item"), f.Request()).has_value());
            REQUIRE(PlotCandidates::Build(SlotOf("plant", "item"), f.Request()).empty());
        }
    }
}

TEST_CASE("PlotCandidates witnesses", "[PlotCandidates][engine]")
{
    Fixture f;

    SECTION("when an earlier step reached somebody")
    {
        PlotModel::Step bribe;
        bribe.verb = "bribe";
        bribe.outcome = PlotModel::StepOutcome::FailedCaught;
        bribe.slots.push_back({"mark", kStranger, "Brand-Shei"});
        PlotModel::Step watch;
        watch.verb = "watch";
        watch.outcome = PlotModel::StepOutcome::Succeeded; // a watch's mark only knows on failure
        watch.slots.push_back({"mark", kNeighbour, "Hulda"});
        f.steps = {bribe, watch};

        SECTION("should offer exactly the people who know")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("silence", "witness"), f.Request()))
                    == std::vector<RE::FormID>{kStranger});
        }
    }

    SECTION("when no earlier step reached anybody")
    {
        SECTION("should fall back to the people the mastermind knows")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("silence", "witness"), f.Request()))
                    == Forms(PlotCandidates::Build(SlotOf("watch", "mark"), f.Request())));
        }
    }

    SECTION("when awareness is checked against an outcome")
    {
        using PlotModel::StepOutcome;
        using PlotVerbs::Awareness;

        SECTION("should fire contact on success or failure, failure only on failure, and nothing on a timeout")
        {
            REQUIRE(PlotCandidates::AwarenessFired(Awareness::Contact, StepOutcome::Succeeded));
            REQUIRE(PlotCandidates::AwarenessFired(Awareness::Contact, StepOutcome::FailedCaught));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Contact, StepOutcome::FailedTimeout));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Failure, StepOutcome::Succeeded));
            REQUIRE(PlotCandidates::AwarenessFired(Awareness::Failure, StepOutcome::FailedCaught));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Failure, StepOutcome::FailedTimeout));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Never, StepOutcome::FailedCaught));
        }
    }
}

TEST_CASE("PlotPopulation leaves children out", "[PlotPopulation][engine]")
{
    EngineMock engine;
    auto* adult = engine.AddNPC(0x00E95001u, "Lars's father");
    auto* child = engine.AddNPC(0x00E95002u, "Lars Battle-Born");
    auto* childRace = engine.AddRace(0x00E95003u, "NordRaceChild", {"ActorTypeNPC"});
    childRace->data.flags.set(RE::RACE_DATA::Flag::kChild);
    engine.SetNPCRace(child, childRace);

    SECTION("when a race carries the child flag")
    {
        SECTION("should read its NPC as a child, and nobody else")
        {
            REQUIRE(NarrativeEngine::PlotPopulation::IsChild(child));
            REQUIRE_FALSE(NarrativeEngine::PlotPopulation::IsChild(adult));
            REQUIRE_FALSE(NarrativeEngine::PlotPopulation::IsChild(nullptr));
        }
    }
}
