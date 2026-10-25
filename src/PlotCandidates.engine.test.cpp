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
        jarl.office = PlotCasting::Office::Jarl;
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
            REQUIRE(list.front().value.text == "Idolaf");
            REQUIRE(list.front().why.relation == "close");
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
            REQUIRE(list.back().value.text == "Brand-Shei");
            REQUIRE(list.back().why.relation.empty());
            REQUIRE(list.back().why.lives == "Riften");
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

    SECTION("when a denunciation's authority is asked for, with authorities in two holds")
    {
        constexpr RE::FormID kBalgruuf = 0x00010009;
        constexpr RE::FormID kFalkreathHold = 0x0000B003;
        constexpr RE::FormID kOutsider = 0x0001000A;
        auto balgruuf = Person(kBalgruuf, "Balgruuf", kWhiterunHold, kWhiterun);
        balgruuf.office = PlotCasting::Office::Jarl;
        f.population.members.push_back(balgruuf);
        f.population.members.push_back(Person(kOutsider, "Dengeir", kFalkreathHold, 0));
        const auto authority = SlotOf("denounce", "authority");

        SECTION("should offer only the mark's own hold's")
        {
            f.filled.push_back({"mark", kStranger, "Brand-Shei"});
            REQUIRE(Forms(PlotCandidates::Build(authority, f.Request())) == std::vector<RE::FormID>{kJarl});
            f.filled.back() = {"mark", kNeighbour, "Hulda"};
            REQUIRE(Forms(PlotCandidates::Build(authority, f.Request())) == std::vector<RE::FormID>{kBalgruuf});
        }

        SECTION("should fall back to the mastermind's hold when the mark's has none")
        {
            f.filled.push_back({"mark", kOutsider, "Dengeir"});
            REQUIRE(Forms(PlotCandidates::Build(authority, f.Request())) == std::vector<RE::FormID>{kBalgruuf});
        }
    }

    SECTION("when the mark belongs to an organisation with people above them in it")
    {
        constexpr RE::FormID kKodlak = 0x0001000B;
        constexpr RE::FormID kAela = 0x0001000C;
        constexpr RE::FormID kTorvar = 0x0001000D;
        auto kodlak = Person(kKodlak, "Kodlak", kWhiterunHold, kWhiterun);
        kodlak.factions.push_back({kCompanions, 1.0, true});
        f.population.members.push_back(kodlak);
        auto aela = Person(kAela, "Aela", kWhiterunHold, kWhiterun);
        aela.factions.push_back({kCompanions, 0.6, true}); // level with the mark
        f.population.members.push_back(aela);
        auto torvar = Person(kTorvar, "Torvar", kWhiterunHold, kWhiterun);
        torvar.factions.push_back({kCompanions, 0.3, true}); // below the mark
        f.population.members.push_back(torvar);
        const auto authority = SlotOf("denounce", "authority");

        SECTION("should offer only those who outrank them there, when their hold has no court")
        {
            f.filled.push_back({"mark", kColleague, "Vilkas"});
            REQUIRE(Forms(PlotCandidates::Build(authority, f.Request())) == std::vector<RE::FormID>{kKodlak});
        }

        SECTION("should offer their hold's court alongside those who outrank them")
        {
            constexpr RE::FormID kBalgruuf = 0x00010009;
            auto balgruuf = Person(kBalgruuf, "Balgruuf", kWhiterunHold, kWhiterun);
            balgruuf.office = PlotCasting::Office::Jarl;
            f.population.members.push_back(balgruuf);
            f.filled.push_back({"mark", kColleague, "Vilkas"});
            const auto list = PlotCandidates::Build(authority, f.Request());
            REQUIRE(list.size() == 2);
            REQUIRE(Has(list, kKodlak));
            REQUIRE(Has(list, kBalgruuf));
        }

        SECTION("should fall through to the hold courts for the one nobody outranks")
        {
            f.filled.push_back({"mark", kKodlak, "Kodlak"});
            REQUIRE(Forms(PlotCandidates::Build(authority, f.Request())) == std::vector<RE::FormID>{kJarl});
        }
    }

    SECTION("when a petition's authority is asked for")
    {
        constexpr RE::FormID kKodlak = 0x0001000B;
        constexpr RE::FormID kBalgruuf = 0x00010009;
        auto balgruuf = Person(kBalgruuf, "Balgruuf", kWhiterunHold, kWhiterun);
        balgruuf.office = PlotCasting::Office::Jarl;
        f.population.members.push_back(balgruuf);
        const auto authority = SlotOf("petition", "authority");

        SECTION("should go to the mastermind's superior where they have one, or their hold's court")
        {
            f.population.members[0].factions[0].standing = 0.6;
            auto kodlak = Person(kKodlak, "Kodlak", kWhiterunHold, kWhiterun);
            kodlak.factions.push_back({kCompanions, 1.0, true});
            f.population.members.push_back(kodlak);
            const auto list = PlotCandidates::Build(authority, f.Request());
            REQUIRE(list.size() == 2);
            REQUIRE(Has(list, kKodlak));
            REQUIRE(Has(list, kBalgruuf));
        }

        SECTION("should go to the mastermind's hold court where nobody outranks them")
        {
            REQUIRE(Forms(PlotCandidates::Build(authority, f.Request())) == std::vector<RE::FormID>{kBalgruuf});
        }
    }

    SECTION("when a denunciation's authority is asked for and neither hold has one")
    {
        constexpr RE::FormID kFalkreathHold = 0x0000B003;
        constexpr RE::FormID kOutsider = 0x0001000A;
        f.population.members.push_back(Person(kOutsider, "Dengeir", kFalkreathHold, 0));
        f.filled.push_back({"mark", kOutsider, "Dengeir"});

        SECTION("should offer every authority there is")
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
            REQUIRE(list.front().value.text == "Hulda");
            REQUIRE(list.front().why.occupation == "runs the Bannered Mare");
            REQUIRE(list.front().why.relation == "neighbour");
            REQUIRE(list.back().value.text == "Brand-Shei");
            REQUIRE(list.back().why.relation.empty());
            REQUIRE(list.back().why.lives == "Riften");
        }
    }

    SECTION("when the scheme names a stranger")
    {
        auto request = f.Request();
        request.naming = "steal Brand-Shei's ledger";
        const auto list = PlotCandidates::Build(SlotOf("watch", "mark"), request);

        SECTION("should admit them to a slot about whom the mastermind knows, first, saying why")
        {
            REQUIRE(list.front().value.form == kStranger);
            REQUIRE(list.front().value.text == "Brand-Shei");
            REQUIRE(list.front().why.lives == "Riften");
            REQUIRE(list.front().why.named);
            REQUIRE(list.size() == 5);
        }
    }

    SECTION("when the scheme names somebody the rule cannot admit")
    {
        auto request = f.Request();
        request.naming = "ruin Vilkas";

        SECTION("should still keep a non-trader off a trader's list")
        {
            REQUIRE_FALSE(Has(PlotCandidates::Build(SlotOf("buy", "seller"), request), kColleague));
        }
    }

    SECTION("when the scheme names somebody a property rule admits")
    {
        auto request = f.Request();
        request.naming = "buy out Brand-Shei";

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
            REQUIRE(list.front().value.text == "a ruby");
        }

        SECTION("should be asked")
        {
            REQUIRE(PlotCandidates::Asks(SlotOf("steal", "item")));
        }
    }

    SECTION("when only a document will do")
    {
        f.world.items = {{0x0000D001, "a ruby", NarrativeEngine::PlotItemPool::Category::Valuable},
                         {0x0000D003, "a merchant's ledger", NarrativeEngine::PlotItemPool::Category::Document},
                         {0x0000D004, "a deed to a property", NarrativeEngine::PlotItemPool::Category::Document}};
        PlotVerbs::Slot paper{"item", PlotVerbs::SlotKind::Thing, PlotVerbs::Rule::DocumentPool};

        SECTION("should offer the pool's documents and nothing else, in pool order")
        {
            REQUIRE(Forms(PlotCandidates::Build(paper, f.Request()))
                    == std::vector<RE::FormID>{0x0000D003, 0x0000D004});
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

        SECTION("should fire contact on success or failure, failure and success only on their own, and nothing on a "
                "timeout")
        {
            REQUIRE(PlotCandidates::AwarenessFired(Awareness::Success, StepOutcome::Succeeded));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Success, StepOutcome::FailedCaught));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Success, StepOutcome::FailedTimeout));
            REQUIRE(PlotCandidates::AwarenessFired(Awareness::Contact, StepOutcome::Succeeded));
            REQUIRE(PlotCandidates::AwarenessFired(Awareness::Contact, StepOutcome::FailedCaught));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Contact, StepOutcome::FailedTimeout));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Failure, StepOutcome::Succeeded));
            REQUIRE(PlotCandidates::AwarenessFired(Awareness::Failure, StepOutcome::FailedCaught));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Failure, StepOutcome::FailedTimeout));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(Awareness::Never, StepOutcome::FailedCaught));
        }

        SECTION("should make a denunciation's accused aware only when it worked")
        {
            const auto accused = SlotOf("denounce", "mark");
            REQUIRE(PlotCandidates::AwarenessFired(accused.awareness, StepOutcome::Succeeded));
            REQUIRE_FALSE(PlotCandidates::AwarenessFired(accused.awareness, StepOutcome::FailedCaught));
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

TEST_CASE("PlotCandidates craft rules", "[PlotCandidates][engine]")
{
    namespace PlotRecipes = NarrativeEngine::PlotRecipes;
    Fixture f;

    constexpr RE::FormID kDagger = 0x0000E001;
    constexpr RE::FormID kSword = 0x0000E002;
    constexpr RE::FormID kDaedric = 0x0000E003;
    constexpr RE::FormID kPotion = 0x0000E004;
    const auto recipe = [](RE::FormID form, std::string name, PlotRecipes::Craft craft, int skill, float value) {
        PlotRecipes::Recipe r;
        r.form = form;
        r.displayName = std::move(name);
        r.craft = craft;
        r.skill = skill;
        r.ingredientValue = value;
        r.ingredientCount = 2;
        return r;
    };
    f.world.recipes = {recipe(kDagger, "an Iron Dagger", PlotRecipes::Craft::Smith, 0, 10.0f),
                       recipe(kSword, "a Steel Sword", PlotRecipes::Craft::Smith, 30, 30.0f),
                       recipe(kDaedric, "a Daedric Sword", PlotRecipes::Craft::Smith, 90, 900.0f),
                       recipe(kPotion, "a Potion of Healing", PlotRecipes::Craft::Concoct, 0, 5.0f)};

    const auto crafts = [&f](RE::FormID npc, PlotRecipes::Craft craft, double skill) {
        for (auto& m : f.population.members) {
            if (m.npc == npc) {
                m.crafts |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(craft));
                m.skills.smithing = skill / 100.0;
                m.skills.alchemy = skill / 100.0;
            }
        }
    };

    SECTION("when the mastermind's friend is a smith, and a better one lives where they cannot reach")
    {
        crafts(kFriend, PlotRecipes::Craft::Smith, 40);
        crafts(kNeighbour, PlotRecipes::Craft::Smith, 100);
        const auto list = PlotCandidates::Build(SlotOf("smith", "item"), f.Request());

        SECTION("should list what the friend is skilled enough to make, and nothing beyond it")
        {
            REQUIRE(Forms(list) == std::vector<RE::FormID>{kDagger, kSword});
            REQUIRE(list[1].value.text == "a Steel Sword");
        }

        SECTION("should list nothing of another craft")
        {
            REQUIRE_FALSE(Has(list, kPotion));
        }
    }

    SECTION("when the scheme names something that can be made")
    {
        crafts(kFriend, PlotRecipes::Craft::Smith, 40);
        const std::string scheme = "Bring a steel sword to the Jarl";
        auto request = f.Request();
        request.naming = scheme;

        SECTION("should offer it first")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("smith", "item"), request)).front() == kSword);
        }
    }

    SECTION("when the mastermind practises the craft themselves")
    {
        crafts(kBoss, PlotRecipes::Craft::Concoct, 15);

        SECTION("should list what they can make")
        {
            REQUIRE(Forms(PlotCandidates::Build(SlotOf("concoct", "item"), f.Request()))
                    == std::vector<RE::FormID>{kPotion});
        }
    }

    SECTION("when nobody the mastermind can reach practises the craft")
    {
        crafts(kStranger, PlotRecipes::Craft::Smith, 100);

        SECTION("should list nothing, which keeps the verb from being offered")
        {
            REQUIRE(PlotCandidates::Build(SlotOf("smith", "item"), f.Request()).empty());
        }
    }

    SECTION("when an item nobody has got is chosen")
    {
        crafts(kFriend, PlotRecipes::Craft::Smith, 40);
        PlotVerbs::Slot slot{"item", PlotVerbs::SlotKind::Thing, PlotVerbs::Rule::PoolOrMade, {}};
        const std::string finish = "Bring a steel sword to Laila";
        auto request = f.Request();
        request.naming = finish;
        const auto list = PlotCandidates::Build(slot, request);

        SECTION("should offer a made thing the scheme names, then the pool, then the rest that can be made")
        {
            REQUIRE(Forms(list) == std::vector<RE::FormID>{kSword, 0x0000D001, 0x0000D002, kDagger});
        }
    }

    SECTION("when the scheme call is shown what can be made")
    {
        crafts(kFriend, PlotRecipes::Craft::Smith, 100);
        crafts(kBoss, PlotRecipes::Craft::Concoct, 15);

        SECTION("should show a spread of each craft, from the cheapest")
        {
            REQUIRE(PlotCandidates::MadeExamples(f.Request(), 2)
                    == std::vector<std::string>{"an Iron Dagger", "a Daedric Sword", "a Potion of Healing"});
        }

        SECTION("should say whether a thing can be made, by its craft")
        {
            REQUIRE(PlotCandidates::CanBeMade(f.Request(), PlotRecipes::Craft::Smith, kDaedric));
            REQUIRE_FALSE(PlotCandidates::CanBeMade(f.Request(), PlotRecipes::Craft::Concoct, kDaedric));
        }
    }

    SECTION("when there are more things than the list holds")
    {
        crafts(kFriend, PlotRecipes::Craft::Smith, 100);
        PlotCandidates::Limits limits;
        limits.things = 2;
        const auto list = PlotCandidates::Build(SlotOf("smith", "item"), f.Request(), limits);

        SECTION("should spread what it keeps from the cheapest to the dearest")
        {
            REQUIRE(Forms(list) == std::vector<RE::FormID>{kDagger, kDaedric});
        }
    }
}

TEST_CASE("PlotCandidates resolves a goal's roles", "[PlotCandidates][engine]")
{
    Fixture f;
    const auto resolve = [&f](std::string_view verb, std::vector<PlotModel::SlotValue> named) {
        const auto* goal = PlotVerbs::Find(verb);
        REQUIRE(goal != nullptr);
        return PlotCandidates::ResolveRoles(*goal, named, f.Request());
    };

    SECTION("when every role names somebody or something the plot can reach")
    {
        const auto out = resolve("steal", {{"item", 0, "the ruby"}, {"mark", 0, "Hulda"}});

        SECTION("should resolve each, in slot order, as it is said")
        {
            REQUIRE(out.problem.empty());
            REQUIRE(out.values.size() == 2);
            REQUIRE(out.values[0].form == 0x0000D001);
            REQUIRE(out.values[0].text == "a ruby");
            REQUIRE(out.values[1].form == kNeighbour);
        }
    }

    SECTION("when a role names somebody the mastermind does not know")
    {
        const auto out = resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Brand-Shei"}});

        SECTION("should refuse them, naming the role, though naming them made them known to a list")
        {
            REQUIRE(out.problem.find("Brand-Shei cannot be the mark (whom it is stolen from)") != std::string::npos);
        }
    }

    SECTION("when a name is behind a title many people carry")
    {
        RE::FormID npc = 0x00010301;
        for (const auto* name : {"Captain Valmir", "Captain Aldis", "Captain Metilius", "Captain Wayfinder"}) {
            f.population.members.push_back(Person(npc++, name, kWhiterunHold, kWhiterun));
        }

        SECTION("should refuse an invented name as nobody, not as any of them")
        {
            const auto out = resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Captain Veiled Lotus"}});
            REQUIRE(out.problem.find("'Captain Veiled Lotus' names nobody in Skyrim") != std::string::npos);
        }

        SECTION("should still resolve a real one")
        {
            const auto out = resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Captain Aldis"}});
            REQUIRE(out.problem.empty());
            REQUIRE(out.values[1].form == 0x00010302);
        }
    }

    SECTION("when a refused role is told who would do")
    {
        SECTION("should list everybody the rule admits when there are few")
        {
            const auto out = resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Mogrul"}});
            REQUIRE(out.problem.find("; it can be ") != std::string::npos);
            REQUIRE(out.problem.find("Idolaf") != std::string::npos);
        }

        SECTION("should list the closest eight when there are more")
        {
            for (RE::FormID npc = 0x00010201; npc < 0x00010209; ++npc) {
                f.population.members.push_back(
                    Person(npc, "Townsfolk " + std::to_string(npc), kWhiterunHold, kWhiterun));
            }
            const auto out = resolve("denounce", {{"mark", 0, "Ghost of Somebody"}, {"authority", 0, "Laila"}});
            const auto at = out.problem.find("; it could be, among others, ");
            REQUIRE(at != std::string::npos);
            // Eight names: six commas between them, then "or" before the last.
            const auto offered = out.problem.substr(at + std::string_view{"; it could be, among others, "}.size());
            REQUIRE(std::count(offered.begin(), offered.end(), ',') == 6);
            REQUIRE(offered.find(" or ") != std::string::npos);
        }
    }

    SECTION("when a role names nobody living")
    {
        f.population.absent = {{"Mogrul", "no home location"}, {"Hulda", "a ghost"}};

        SECTION("should say when they are dead or not in the world")
        {
            REQUIRE(resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Ghost of Somebody"}})
                        .problem.find("Ghost of Somebody is dead or not in the world, for the mark")
                    != std::string::npos);
        }

        SECTION("should say when the plots leave them out, and why")
        {
            REQUIRE(resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Mogrul"}})
                        .problem.find("Mogrul cannot take part in these plots (no home location), for the mark")
                    != std::string::npos);
        }

        SECTION("should say when nobody in Skyrim has the name")
        {
            REQUIRE(resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Jarl Vigilia Rulcanella"}})
                        .problem.find("'Jarl Vigilia Rulcanella' names nobody in Skyrim, for the mark")
                    != std::string::npos);
        }

        SECTION("should say a role takes one person's name when the words are a group's")
        {
            REQUIRE(resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "the East Empire Company"}})
                        .problem.find("; a person's role takes one person's name, never a group's or a place's")
                    != std::string::npos);
        }

        SECTION("should say when the role names the mastermind")
        {
            REQUIRE(resolve("denounce", {{"mark", 0, "Hulda"}, {"authority", 0, "Olfrid"}})
                        .problem.find("Olfrid is the one scheming and cannot be in their own scheme, for the authority")
                    != std::string::npos);
        }

        SECTION("should still find the living person when a left-out record shares their name")
        {
            const auto out = resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Hulda"}});
            REQUIRE(out.problem.empty());
            REQUIRE(out.values[1].form == kNeighbour);
        }
    }

    SECTION("when two people carry the name a role gives")
    {
        constexpr RE::FormID kElder = 0x00010101;
        constexpr RE::FormID kYounger = 0x00010102;
        f.population.members.push_back(Person(kElder, "Idgrod Ravencrone", kWhiterunHold, kWhiterun));
        f.population.members.push_back(Person(kYounger, "Idgrod the Younger", kWhiterunHold, kWhiterun));

        SECTION("should ask for the whole name when only the shared word is given")
        {
            const auto out = resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Jarl Idgrod"}});
            REQUIRE(out.problem.find("could be Idgrod Ravencrone or Idgrod the Younger") != std::string::npos);
        }

        SECTION("should resolve the whole name")
        {
            const auto out = resolve("steal", {{"item", 0, "a ruby"}, {"mark", 0, "Idgrod Ravencrone"}});
            REQUIRE(out.problem.empty());
            REQUIRE(out.values[1].form == kElder);
        }
    }

    SECTION("when a denunciation names an authority")
    {
        SECTION("should take one the rule admits")
        {
            const auto out = resolve("denounce", {{"mark", 0, "Hulda"}, {"authority", 0, "Laila"}});
            REQUIRE(out.problem.empty());
            REQUIRE(out.values[1].form == kJarl);
        }

        SECTION("should refuse one the rule passes over, saying whom the mark answers to")
        {
            auto balgruuf = Person(0x00010009, "Balgruuf the Greater", kWhiterunHold, kWhiterun);
            balgruuf.office = PlotCasting::Office::Jarl;
            f.population.members.push_back(balgruuf);
            const auto out = resolve("denounce", {{"mark", 0, "Hulda"}, {"authority", 0, "Laila Law-Giver"}});
            REQUIRE(out.problem.find("Laila Law-Giver cannot be the authority (whom they are denounced to); it can be "
                                     "Balgruuf the Greater")
                    != std::string::npos);
        }
    }

    SECTION("when the mark is not named")
    {
        const auto out = resolve("denounce", {{"authority", 0, "Laila"}});

        SECTION("should say so, and not judge the authority against nobody")
        {
            REQUIRE(out.problem == "nothing was named as the mark (who is denounced)");
        }
    }

    SECTION("when a thing is named that nothing can be")
    {
        const auto out = resolve("steal", {{"item", 0, "a spellbook"}, {"mark", 0, "Hulda"}});

        SECTION("should say so, listing what can be")
        {
            REQUIRE(out.problem.find("'a spellbook' names nothing that can be the item") != std::string::npos);
            REQUIRE(out.problem.find("a ruby; a bottle of skooma") != std::string::npos);
        }
    }

    SECTION("when a place is named")
    {
        const auto out = resolve("spread_rumors", {{"mark", 0, "Hulda"}, {"place", 0, "the Whiterun market"}});

        SECTION("should resolve the settlement it holds")
        {
            REQUIRE(out.problem.empty());
            REQUIRE(out.values[1].form == kWhiterun);
        }
    }
}

TEST_CASE("PlotCandidates ranks authority", "[PlotCandidates][engine]")
{
    using PlotCasting::Office;
    Fixture f;
    const auto authority = SlotOf("denounce", "authority");

    constexpr RE::FormID kBalgruuf = 0x00010401;
    constexpr RE::FormID kProventus = 0x00010402;
    constexpr RE::FormID kCaius = 0x00010403;
    constexpr RE::FormID kWhiterunCourt = 0x0000C101;
    const auto official = [&f](RE::FormID npc, const char* name, Office office) {
        auto m = Person(npc, name, kWhiterunHold, kWhiterun);
        m.office = office;
        f.population.members.push_back(m);
    };
    official(kBalgruuf, "Balgruuf the Greater", Office::Jarl);
    official(kProventus, "Proventus Avenicci", Office::Steward);
    official(kCaius, "Caius", Office::GuardCaptain);
    const auto against = [&f, &authority](RE::FormID mark, std::string name) {
        f.filled = {{"mark", mark, std::move(name)}};
        return PlotCandidates::Build(authority, f.Request());
    };

    SECTION("when a steward is accused")
    {
        SECTION("should go to the jarl, and never to the guard captain beneath them")
        {
            REQUIRE(Forms(against(kProventus, "Proventus Avenicci")) == std::vector<RE::FormID>{kBalgruuf});
        }
    }

    SECTION("when a guard captain is accused")
    {
        SECTION("should go to those above them in the court")
        {
            const auto list = against(kCaius, "Caius");
            REQUIRE(Has(list, kBalgruuf));
            REQUIRE(Has(list, kProventus));
            REQUIRE_FALSE(Has(list, kCaius));
        }
    }

    SECTION("when a jarl is accused")
    {
        SECTION("should be made to the court they lead, with no person in the slot")
        {
            f.population.members.back().factions.clear();
            for (auto& m : f.population.members) {
                if (m.npc == kBalgruuf) {
                    m.factions.push_back({kWhiterunCourt, 1.0, true});
                }
            }
            f.world.factions = {{kWhiterunCourt, "the court of Whiterun"}};
            const auto list = against(kBalgruuf, "Balgruuf the Greater");
            REQUIRE(list.size() == 1);
            REQUIRE(list[0].value.form == 0);
            REQUIRE(list[0].value.text == "the court of Whiterun");
        }

        SECTION("should be made to the people of their town when they lead no listed organisation")
        {
            const auto list = against(kBalgruuf, "Balgruuf the Greater");
            REQUIRE(list.size() == 1);
            REQUIRE(list[0].value.text == "the people of Whiterun");
        }

        SECTION("should resolve a role naming a person as their authority to the court instead")
        {
            const auto* goal = PlotVerbs::Find("denounce");
            const auto out = PlotCandidates::ResolveRoles(
                *goal, {{"mark", 0, "Balgruuf the Greater"}, {"authority", 0, "Proventus Avenicci"}}, f.Request());
            REQUIRE(out.problem.empty());
            REQUIRE(out.values[1].form == 0);
            REQUIRE(out.values[1].text == "the people of Whiterun");
        }
    }

    SECTION("when a jarl would petition")
    {
        for (auto& m : f.population.members) {
            if (m.npc == kBoss) {
                m.office = Office::Jarl;
            }
        }

        SECTION("should have nobody to petition")
        {
            REQUIRE(PlotCandidates::Build(SlotOf("petition", "authority"), f.Request()).empty());
        }
    }

    SECTION("when somebody of an orc stronghold is accused")
    {
        constexpr RE::FormID kStronghold = 0x0000A0F1;
        constexpr RE::FormID kBurguk = 0x00010411;
        constexpr RE::FormID kLob = 0x00010412;
        auto chief = Person(kBurguk, "Chief Burguk", kWhiterunHold, kStronghold);
        chief.office = Office::OrcChief;
        f.population.members.push_back(chief);
        f.population.members.push_back(Person(kLob, "Lob", kWhiterunHold, kStronghold));

        SECTION("should go to its chief, not to the hold's court")
        {
            REQUIRE(Forms(against(kLob, "Lob")) == std::vector<RE::FormID>{kBurguk});
        }

        SECTION("should never go to a chief for somebody outside the stronghold")
        {
            REQUIRE_FALSE(Has(against(kNeighbour, "Hulda"), kBurguk));
        }
    }
}
