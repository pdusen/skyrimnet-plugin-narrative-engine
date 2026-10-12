#include <PlotPlanning.h>

#include <PlotBirth.h>
#include <PlotStepParse.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

// Tests for planning a scheme by decision, backwards from its goal.
//
// The decision call is scripted: each test says what the model answers, in
// order, and the suite asserts what the plan can and cannot become. What
// matters is that nothing reaches a plan that was not on a list the planner
// built -- not a verb, not a person, not an item -- that the plan reads in
// an order that makes sense whatever the model answers, that a model which
// answers the same thing every time cannot fill a plan with repeats, and
// that every way the planning can end, ends it.

namespace
{
    namespace PlotPlanning = NarrativeEngine::PlotPlanning;
    namespace PlotCasting = NarrativeEngine::PlotCasting;
    namespace PlotMenus = NarrativeEngine::PlotMenus;
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;

    constexpr RE::FormID kWhiterun = 0x0000A001;
    constexpr RE::FormID kRiften = 0x0000A002;
    constexpr RE::FormID kHold = 0x0000B001;
    constexpr RE::FormID kRift = 0x0000B002;
    constexpr RE::FormID kBoss = 0x00010001;
    constexpr RE::FormID kFriend = 0x00010002;
    constexpr RE::FormID kTrader = 0x00010003;
    constexpr RE::FormID kJarl = 0x00010004;
    constexpr RE::FormID kRuby = 0x0000D001;
    constexpr RE::FormID kSkooma = 0x0000D002;
    constexpr RE::FormID kDeed = 0x0000D003;

    // Olfrid schemes against Hulda, who trades in Whiterun. He knows Idolaf.
    // Laila is a stranger to him, and the only authority.
    PlotCasting::Population Province()
    {
        PlotCasting::Population p;
        PlotCasting::Member boss;
        boss.npc = kBoss;
        boss.name = "Olfrid";
        boss.hold = kHold;
        boss.settlement = kWhiterun;
        boss.ties.push_back({kFriend, false, 0});
        p.members.push_back(boss);

        PlotCasting::Member friendly;
        friendly.npc = kFriend;
        friendly.name = "Idolaf";
        friendly.hold = kHold;
        friendly.settlement = kWhiterun;
        p.members.push_back(friendly);

        PlotCasting::Member trader;
        trader.npc = kTrader;
        trader.name = "Hulda";
        trader.hold = kHold;
        trader.settlement = kWhiterun;
        trader.tradesForLiving = true;
        p.members.push_back(trader);

        PlotCasting::Member jarl;
        jarl.npc = kJarl;
        jarl.name = "Laila";
        jarl.hold = kRift;
        jarl.settlement = kRiften;
        jarl.office = PlotCasting::Office::Jarl;
        p.members.push_back(jarl);
        return p;
    }

    PlotMenus::World World()
    {
        PlotMenus::World w;
        w.settlements = {{kWhiterun, "Whiterun"}, {kRiften, "Riften"}};
        w.items = {{kRuby, "a ruby", {}},
                   {kSkooma, "a bottle of skooma", {}},
                   {kDeed, "a deed to a property", NarrativeEngine::PlotItemPool::Category::Document}};
        return w;
    }

    // A decision model that answers from a list, and remembers what it was
    // asked.
    struct Script
    {
        struct Asked
        {
            std::string templateName;
            nlohmann::json ctx;
        };

        std::deque<std::string> answers;
        std::vector<Asked> asked;
        std::string failWith; // when set, every request fails with this code

        SkyrimNet::DecisionResult operator()(std::string_view name, const nlohmann::json& ctx)
        {
            asked.push_back({std::string{name}, ctx});
            SkyrimNet::DecisionResult r;
            if (!failWith.empty()) {
                r.error = failWith;
                return r;
            }
            REQUIRE_FALSE(answers.empty());
            SkyrimNet::DecisionAnswer a;
            a.type = "choice";
            a.choice = answers.front();
            answers.pop_front();
            r.answers.emplace_back(QuestionOf(name), a);
            r.ok = true;
            return r;
        }

        // Each template asks its one question under its own id.
        static const char* QuestionOf(std::string_view name)
        {
            if (name == PlotPlanning::kGoalTemplate) {
                return PlotPlanning::kGoalQuestion;
            }
            if (name == PlotPlanning::kPrepareTemplate) {
                return PlotPlanning::kPrepareQuestion;
            }
            if (name == PlotPlanning::kObtainTemplate) {
                return PlotPlanning::kObtainQuestion;
            }
            if (name == PlotPlanning::kAdaptTemplate) {
                return PlotPlanning::kAdaptQuestion;
            }
            return PlotPlanning::kSlotQuestion;
        }

        // The keys offered by the request made at `index`.
        std::vector<std::string> Offered(std::size_t index) const
        {
            std::vector<std::string> out;
            for (const auto& o : asked.at(index).ctx["options"]) {
                out.push_back(o["key"].get<std::string>());
            }
            return out;
        }

        // The lines offered by the request made at `index`.
        std::vector<std::string> Lines(std::size_t index) const
        {
            std::vector<std::string> out;
            for (const auto& o : asked.at(index).ctx["options"]) {
                out.push_back(o["line"].get<std::string>());
            }
            return out;
        }

        std::vector<std::string> Templates() const
        {
            std::vector<std::string> out;
            for (const auto& a : asked) {
                out.push_back(a.templateName);
            }
            return out;
        }
    };

    struct Fixture
    {
        PlotCasting::Population population = Province();
        PlotMenus::World world = World();
        std::vector<PlotModel::Step> history;
        std::string goal;
        std::string finish;
        std::vector<PlotModel::Abandoned> abandoned;
        const PlotModel::Step* goalStep = nullptr;
        std::vector<PlotModel::SlotValue> goalRoles;
        Script script;

        PlotPlanning::Outcome Run(std::size_t maxSteps = PlotPlanning::kPlanCap)
        {
            PlotPlanning::Input input{population, world};
            input.goal = goal;
            input.finish = finish;
            input.abandoned = abandoned;
            input.goalStep = goalStep;
            input.goalRoles = goalRoles;
            input.mastermind = kBoss;
            input.mastermindName = "Olfrid";
            input.ambition = "He means to own Whiterun.";
            input.scheme = "Ruin Hulda's trade.";
            input.maxSteps = maxSteps;
            for (const auto& step : history) {
                input.before.push_back(&step);
            }
            return PlotPlanning::Plan(input,
                                      [this](std::string_view n, const nlohmann::json& c) { return script(n, c); });
        }
    };

    bool Contains(const std::vector<std::string>& keys, std::string_view key)
    {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    }

    RE::FormID FormOf(const PlotModel::Step& step, std::string_view slot)
    {
        const auto* value = step.FindSlot(slot);
        return value != nullptr ? value->form : 0;
    }

    // A `roles` object naming every slot of `goal`, as a scheme call would.
    nlohmann::json RolesFor(std::string_view goal)
    {
        nlohmann::json roles = nlohmann::json::object();
        if (const auto* verb = NarrativeEngine::PlotVerbs::Find(goal)) {
            for (std::size_t i = 0; i < verb->slotCount; ++i) {
                const auto kind = verb->slots[i].kind;
                roles[std::string{verb->slots[i].name}] = kind == NarrativeEngine::PlotVerbs::SlotKind::Person ? "Hulda"
                                                          : kind == NarrativeEngine::PlotVerbs::SlotKind::Place
                                                              ? "Whiterun"
                                                              : "a ruby";
            }
        }
        return roles;
    }

    const std::string kGoal = PlotPlanning::kGoalTemplate;
    const std::string kSlot = PlotPlanning::kSlotTemplate;
    const std::string kPrepare = PlotPlanning::kPrepareTemplate;
    const std::string kObtain = PlotPlanning::kObtainTemplate;
} // namespace

TEST_CASE("PlotPlanning asks for the goal first", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the goal is a theft and one of two preparations is taken")
    {
        // goal steal -> the ruby -> from Hulda (named, first). Prepare p2:
        // ask around about Hulda, whose place is Whiterun alone. Then the
        // watch is still on offer, with ready beside it.
        f.script.answers = {"steal", "c1", "c1", "p2", "ready"};
        const auto out = f.Run();

        SECTION("should ask the goal, its slots, then what comes first")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot, kSlot, kPrepare, kPrepare});
            REQUIRE(out.decisions == 5);
        }

        SECTION("should put the goal last and the preparation before it")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "ask_around");
            REQUIRE(out.plan[1].verb == "steal");
        }

        SECTION("should bind the preparation to the person its step is about, without asking")
        {
            REQUIRE(FormOf(out.plan[1], "mark") == kTrader);
            REQUIRE(FormOf(out.plan[0], "mark") == kTrader);
            REQUIRE(FormOf(out.plan[0], "place") == kWhiterun);
        }

        SECTION("should offer each preparation as a whole step, saying where it goes")
        {
            REQUIRE(f.script.Lines(3)
                    == std::vector<std::string>{"Before step 1: Someone watches Hulda, learning their habits.",
                                                "Before step 1: Someone asks around [place] about Hulda."});
            REQUIRE(f.script.asked[3].ctx["plan"][0] == "1. Someone steals a ruby from Hulda.");
        }

        SECTION("should fill the goal's slots knowing it is the goal")
        {
            REQUIRE(f.script.asked[1].ctx["step"] == "Someone steals [item] from [mark].");
            REQUIRE(f.script.asked[1].ctx["purpose"] == "This step accomplishes the scheme.");
        }

        SECTION("should leave the agent for dispatch")
        {
            REQUIRE(out.plan[1].actor == 0);
            REQUIRE(out.plan[1].FindSlot("mastermind")->form == kBoss);
            REQUIRE(out.plan[1].target == kTrader);
        }
    }

    SECTION("when the goal can be prepared only one way")
    {
        // goal watch -> Hulda. Asking around about her is the only
        // preparation, and with one step planned there is no ready yet.
        f.script.answers = {"watch", "c1"};
        const auto out = f.Run();

        SECTION("should plan it without a question of one option")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot});
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "ask_around");
        }
    }

    SECTION("when the scheme was written to end in a goal")
    {
        f.goal = "watch";
        f.script.answers = {"c1"};
        const auto out = f.Run();

        SECTION("should plan it last without asking for it")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kSlot});
            REQUIRE(out.ok);
            REQUIRE(out.plan.back().verb == "watch");
        }
    }

    SECTION("when how the scheme finishes names somebody the scheme does not")
    {
        // The scheme names Hulda; the finish names Idolaf, so both head the
        // mark list ahead of anybody named by neither.
        f.goal = "watch";
        f.finish = "watch Idolaf";
        f.script.answers = {"c2", "p1"};
        const auto out = f.Run();

        SECTION("should put the people it names on the list first, and tell the question how it ends")
        {
            const auto lines = f.script.Lines(0);
            REQUIRE(lines.size() == 2);
            REQUIRE(lines[0].find("named in the scheme") != std::string::npos);
            REQUIRE(lines[1].find("named in the scheme") != std::string::npos);
            REQUIRE(f.script.asked[0].ctx["finish"] == "watch Idolaf");
        }
    }

    SECTION("when the goal the scheme ends in cannot be filled")
    {
        // Nobody has standing in a rostered faction, so nobody can be bribed.
        f.goal = "bribe";
        f.script.answers = {"watch", "c1", "p1"};
        const auto out = f.Run();

        SECTION("should ask for a goal instead")
        {
            REQUIRE(f.script.asked[0].templateName == kGoal);
            REQUIRE(out.ok);
            REQUIRE(out.plan.back().verb == "watch");
        }
    }

    SECTION("when the goal is answered with a verb that was not offered")
    {
        f.script.answers = {"conquer"};
        const auto out = f.Run();

        SECTION("should reject the plan rather than run it")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.plan.empty());
            REQUIRE(out.rejection.find("conquer") != std::string::npos);
        }
    }

    SECTION("when a slot of the goal is answered with a key that was not offered")
    {
        f.script.answers = {"watch", "c9"};
        const auto out = f.Run();

        SECTION("should reject the plan")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.plan.empty());
        }
    }

    SECTION("when a decision cannot be asked")
    {
        f.script.failWith = "no_decisions_route";
        const auto out = f.Run();

        SECTION("should abort with the error, spending nothing")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.plan.empty());
            REQUIRE(out.error == "no_decisions_route");
            REQUIRE(PlotPlanning::SessionCannotDecide(out.error));
            REQUIRE(f.script.asked.size() == 1);
        }
    }
}

TEST_CASE("PlotPlanning gets the items a plan needs", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the goal delivers something to the person the scheme names")
    {
        // goal deliver -> item: ruby -> recipient: Hulda. Only a theft can
        // get the ruby from anybody but Hulda, so it is planned unasked,
        // from Idolaf, the one other person Olfrid knows.
        f.script.answers = {"deliver", "c1", "c1", "ready"};
        const auto out = f.Run();

        SECTION("should choose the item from the pool, since nobody has one yet")
        {
            REQUIRE(f.script.asked[1].ctx["slot"] == "item");
            REQUIRE(f.script.Offered(1) == std::vector<std::string>{"c1", "c2", "c3"});
        }

        SECTION("should put the theft of that very item just before the delivery")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "steal");
            REQUIRE(out.plan[1].verb == "deliver");
            REQUIRE(FormOf(out.plan[0], "item") == kRuby);
            REQUIRE(FormOf(out.plan[1], "item") == kRuby);
        }

        SECTION("should never get it from the person it is delivered to")
        {
            REQUIRE(FormOf(out.plan[1], "recipient") == kTrader);
            REQUIRE(FormOf(out.plan[0], "mark") == kFriend);
        }

        SECTION("should ask nothing about how, when there is only one way")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot, kSlot, kPrepare});
        }
    }

    SECTION("when the item could be stolen or bought")
    {
        // recipient: Idolaf this time, which leaves Hulda free to be stolen
        // from or bought from.
        f.script.answers = {"deliver", "c1", "c2", "buy", "ready"};
        const auto out = f.Run();

        SECTION("should ask which, naming the item and the step it is for")
        {
            REQUIRE(f.script.asked[3].templateName == kObtain);
            REQUIRE(f.script.Offered(3) == std::vector<std::string>{"steal", "buy"});
            REQUIRE(f.script.Lines(3)[1] == "Someone buys a ruby from [seller].");
            REQUIRE(f.script.asked[3].ctx["need"]["step"] == 1);
            REQUIRE(f.script.asked[3].ctx["need"]["item"] == "a ruby");
        }

        SECTION("should plan the way chosen")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan[0].verb == "buy");
            REQUIRE(FormOf(out.plan[0], "seller") == kTrader);
            REQUIRE(FormOf(out.plan[1], "recipient") == kFriend);
        }
    }

    SECTION("when an earlier step of the plot already got the item")
    {
        PlotModel::Step stolen;
        stolen.verb = "steal";
        stolen.slots = {{"item", kRuby, "a ruby"}, {"mark", kFriend, "Idolaf"}};
        stolen.actor = kBoss;
        stolen.outcome = PlotModel::StepOutcome::Succeeded;
        f.history = {stolen};
        f.script.answers = {"deliver", "c1"};
        const auto out = f.Run();

        SECTION("should bind it rather than ask for an item or get it again")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 1);
            REQUIRE(out.plan[0].verb == "deliver");
            REQUIRE(FormOf(out.plan[0], "item") == kRuby);
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot});
            REQUIRE(f.script.asked[1].ctx["slot"] == "recipient");
        }
    }

    SECTION("when the earlier attempt to get it failed")
    {
        PlotModel::Step caught;
        caught.verb = "steal";
        caught.slots = {{"item", kRuby, "a ruby"}, {"mark", kFriend, "Idolaf"}};
        caught.actor = kBoss;
        caught.outcome = PlotModel::StepOutcome::FailedCaught;
        f.history = {caught};
        f.script.answers = {"deliver", "c1", "c1", "ready"};
        const auto out = f.Run();

        SECTION("should choose an item and get it, since nobody has one")
        {
            REQUIRE(out.ok);
            REQUIRE(f.script.asked[1].ctx["slot"] == "item");
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "steal");
        }
    }

    SECTION("when a preparation needs an item of its own")
    {
        // goal denounce -> mark: Hulda (named) -> authority: Laila, the one
        // authority. Prepare p1 is planting something on Hulda; its item is
        // the skooma, and a theft of it from Idolaf goes in front of it.
        f.script.answers = {"denounce", "c1", "p1", "c2", "ready"};
        const auto out = f.Run();

        SECTION("should offer the planting first among the denunciation's preparations")
        {
            REQUIRE(f.script.Lines(2).front() == "Before step 1: Someone hides [item] among Hulda's belongings.");
        }

        SECTION("should plan the theft, the planting, then the denunciation")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 3);
            REQUIRE(out.plan[0].verb == "steal");
            REQUIRE(out.plan[1].verb == "plant");
            REQUIRE(out.plan[2].verb == "denounce");
        }

        SECTION("should plant on the person denounced, and steal from somebody else")
        {
            REQUIRE(FormOf(out.plan[1], "mark") == kTrader);
            REQUIRE(FormOf(out.plan[2], "mark") == kTrader);
            REQUIRE(FormOf(out.plan[0], "mark") == kFriend);
            REQUIRE(FormOf(out.plan[0], "item") == kSkooma);
            REQUIRE(FormOf(out.plan[1], "item") == kSkooma);
        }

        SECTION("should tell the item's question what the planting is for")
        {
            REQUIRE(f.script.asked[3].ctx["purpose"] == "It prepares for step 1: Someone denounces Hulda to Laila.");
        }
    }

    SECTION("when the plan has room for only one step")
    {
        f.script.answers = {"watch", "c1"};
        (void)f.Run(1);

        SECTION("should not offer a goal that needs an item, which needs a second step to get")
        {
            REQUIRE_FALSE(Contains(f.script.Offered(0), "deliver"));
            REQUIRE_FALSE(Contains(f.script.Offered(0), "plant"));
            REQUIRE(Contains(f.script.Offered(0), "steal"));
        }
    }
}

TEST_CASE("PlotPlanning acts on the people the finish names", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the scheme call named the goal's roles")
    {
        // The case that went wrong: a finish naming only whom the proof is
        // shown to. The roles say whom it is against. The deed is then dug
        // up against her; then ready.
        f.goal = "expose";
        f.finish = "Show a deed to a property to Laila";
        f.goalRoles = {
            {"item", kDeed, "a deed to a property"}, {"mark", kTrader, "Hulda"}, {"authority", kJarl, "Laila"}};
        f.script.answers = {"dig_up", "ready"};
        const auto out = f.Run();

        SECTION("should plan the goal as they say, asking nothing about it")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kObtain, kPrepare});
            REQUIRE(out.ok);
            REQUIRE(out.plan.back().verb == "expose");
            REQUIRE(FormOf(out.plan.back(), "mark") == kTrader);
            REQUIRE(FormOf(out.plan.back(), "authority") == kJarl);
            REQUIRE(FormOf(out.plan.back(), "item") == kDeed);
        }
    }

    SECTION("when the roles can no longer be filled")
    {
        // The mark has died since the scheme call. The goal is still a theft;
        // what it is of, and from whom, is asked again.
        f.goal = "steal";
        f.goalRoles = {{"item", kRuby, "a ruby"}, {"mark", 0x0001FFFF, "Somebody Gone"}};
        f.script.answers = {"c1", "c1", "p1", "ready"};
        const auto out = f.Run();

        SECTION("should fill the goal's slots by asking, as before")
        {
            REQUIRE_FALSE(f.script.asked.empty());
            REQUIRE(f.script.asked[0].templateName == kSlot);
            REQUIRE(out.ok);
            REQUIRE(FormOf(out.plan.back(), "mark") != 0x0001FFFF);
        }
    }

    SECTION("when a plan is made again after a failure")
    {
        // The goal was a theft of the ruby from Idolaf. A re-plan keeps it,
        // and only prepares it again.
        PlotModel::Step kept;
        kept.verb = "steal";
        kept.slots = {{"mastermind", kBoss, "Olfrid"}, {"item", kRuby, "a ruby"}, {"mark", kFriend, "Idolaf"}};
        f.goalStep = &kept;
        f.script.answers = {"p1", "ready"};
        const auto out = f.Run();

        SECTION("should keep every slot of the goal, and ask neither the goal nor whom")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kPrepare, kPrepare});
            REQUIRE(out.ok);
            REQUIRE(out.plan.back().verb == "steal");
            REQUIRE(FormOf(out.plan.back(), "item") == kRuby);
            REQUIRE(FormOf(out.plan.back(), "mark") == kFriend);
        }
    }

    SECTION("when a planting before a denunciation needs a document")
    {
        // goal denounce -> Hulda (c1). Prepare p1, the planting; its item is
        // the deed (c3), falsified.
        f.script.answers = {"denounce", "c1", "p1", "c3", "falsify", "ready"};
        const auto out = f.Run();

        SECTION("should forge it in the name of the person it is planted on, without asking")
        {
            REQUIRE(f.script.asked.size() == 6);
            REQUIRE(f.script.Lines(4)[1] == "Someone falsifies a deed to a property in Hulda's name.");
            REQUIRE(out.ok);
            REQUIRE(out.plan[0].verb == "falsify");
            REQUIRE(FormOf(out.plan[0], "subject") == kTrader);
        }
    }
}

TEST_CASE("PlotPlanning exposes somebody with proof", "[PlotPlanning][engine]")
{
    Fixture f;
    // goal expose -> the deed, the pool's one document (c3) -> against Hulda
    // (c1) -> to Laila, the one authority, without asking.
    f.finish = "Show a deed to a property to Laila as proof against Hulda";

    SECTION("when the proof is dug up")
    {
        f.script.answers = {"expose", "c3", "c1", "dig_up", "ready"};
        const auto out = f.Run();

        SECTION("should offer taking it from the person exposed, buying, forging or digging it up")
        {
            REQUIRE(f.script.asked[3].templateName == kObtain);
            REQUIRE(f.script.Offered(3) == std::vector<std::string>{"steal", "buy", "falsify", "dig_up"});
            REQUIRE(f.script.Lines(3)[3] == "Someone digs up a deed to a property proving Hulda's wrongdoing.");
        }

        SECTION("should dig it up against the person exposed, then show it")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "dig_up");
            REQUIRE(FormOf(out.plan[0], "mark") == kTrader);
            REQUIRE(out.plan[1].verb == "expose");
            REQUIRE(FormOf(out.plan[1], "mark") == kTrader);
            REQUIRE(FormOf(out.plan[1], "authority") == kJarl);
            REQUIRE(PlotPlanning::Serves(out.plan[0], out.plan[1]));
        }
    }

    SECTION("when the proof is stolen")
    {
        f.script.answers = {"expose", "c3", "c1", "steal", "c1", "ready"};
        const auto out = f.Run();

        SECTION("should offer the person exposed as the one to rob, and never the authority")
        {
            REQUIRE(f.script.asked[4].templateName == kSlot);
            REQUIRE(Contains(f.script.Offered(4), "c1"));
            REQUIRE(out.ok);
            REQUIRE(FormOf(out.plan[0], "mark") == kTrader);
            for (const auto& line : f.script.Lines(4)) {
                REQUIRE(line.find("Laila") == std::string::npos);
            }
        }
    }
}

TEST_CASE("PlotPlanning ends a scheme only in what can finish it", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the goal is asked for")
    {
        f.script.answers = {"watch", "c1", "p1", "ready"};
        (void)f.Run();

        SECTION("should never offer making, finding or hiding something")
        {
            REQUIRE(f.script.asked[0].templateName == kGoal);
            for (const auto* id : {"falsify", "dig_up", "plant", "smith", "concoct"}) {
                INFO(id);
                REQUIRE_FALSE(Contains(f.script.Offered(0), id));
            }
        }
    }

    SECTION("when the goal is a rumor")
    {
        // goal spread_rumors -> Hulda (c1), whose place is Whiterun alone.
        // Prepare p1, the watch; then ready.
        f.script.answers = {"spread_rumors", "c1", "p1", "ready"};
        (void)f.Run();

        SECTION("should offer hiding something among their belongings first, for the talk to lead people to")
        {
            const auto lines = f.script.Lines(2);
            REQUIRE(
                std::count(lines.begin(), lines.end(), "Before step 1: Someone hides [item] among Hulda's belongings.")
                == 1);
        }
    }
}

TEST_CASE("PlotPlanning may falsify a document", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the goal delivers a document nobody holds")
    {
        // goal deliver -> the deed (c3) -> to Idolaf (c2). Hulda could be
        // robbed of it or sell it, and it can be falsified or dug up;
        // falsified, in Hulda's name (c1).
        f.script.answers = {"deliver", "c3", "c2", "falsify", "c1", "ready"};
        const auto out = f.Run();

        SECTION("should offer falsifying it beside stealing, buying and digging it up")
        {
            REQUIRE(f.script.asked[3].templateName == kObtain);
            REQUIRE(f.script.Offered(3) == std::vector<std::string>{"steal", "buy", "falsify", "dig_up"});
            REQUIRE(f.script.Lines(3)[2] == "Someone falsifies a deed to a property in [subject]'s name.");
        }

        SECTION("should plan the forgery to supply the delivery")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "falsify");
            REQUIRE(FormOf(out.plan[0], "item") == kDeed);
            REQUIRE(FormOf(out.plan[0], "subject") == kTrader);
            REQUIRE(PlotPlanning::Serves(out.plan[0], out.plan[1]));
        }
    }

    SECTION("when the goal delivers something that is not paper")
    {
        f.script.answers = {"deliver", "c1", "c2", "buy", "ready"};
        (void)f.Run();

        SECTION("should not offer to falsify it")
        {
            REQUIRE(f.script.Offered(3) == std::vector<std::string>{"steal", "buy"});
        }
    }

    SECTION("when the goal is a denunciation")
    {
        f.script.answers = {"denounce", "c1", "p1", "c2", "ready"};
        (void)f.Run();

        SECTION("should offer digging up proof against the person denounced")
        {
            const auto lines = f.script.Lines(2);
            REQUIRE(std::count(
                        lines.begin(), lines.end(), "Before step 1: Someone digs up [item] proving Hulda's wrongdoing.")
                    == 1);
        }
    }
}

TEST_CASE("PlotPlanning may have a thing made", "[PlotPlanning][engine]")
{
    namespace PlotRecipes = NarrativeEngine::PlotRecipes;
    constexpr RE::FormID kNecklace = 0x0000E001;
    constexpr RE::FormID kPoison = 0x0000E002;

    Fixture f;
    const auto recipe = [](RE::FormID form, std::string name, PlotRecipes::Craft craft) {
        PlotRecipes::Recipe r;
        r.form = form;
        r.displayName = std::move(name);
        r.craft = craft;
        r.ingredientValue = 100.0f;
        r.ingredientCount = 3;
        return r;
    };
    f.world.recipes = {recipe(kNecklace, "a Gold Diamond Necklace", PlotRecipes::Craft::Set),
                       recipe(kPoison, "a Poison of Lingering Damage", PlotRecipes::Craft::Concoct)};
    const auto crafts = [&f](RE::FormID npc, PlotRecipes::Craft craft) {
        for (auto& m : f.population.members) {
            if (m.npc == npc) {
                m.crafts |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(craft));
            }
        }
    };
    // Idolaf, whom Olfrid can send, is a jeweler; Olfrid concocts himself.
    crafts(kFriend, PlotRecipes::Craft::Set);
    crafts(kBoss, PlotRecipes::Craft::Concoct);

    SECTION("when the scheme ends in a gift of something a jeweler can make")
    {
        // goal deliver -> the necklace, named in the finish (c1) -> to Laila
        // (c2), after Hulda whom the scheme names. It could be stolen, bought
        // from Hulda, or made.
        f.finish = "Bring a gold diamond necklace to Laila";
        f.script.answers = {"deliver", "c1", "c2", "set"};
        const auto out = f.Run();

        SECTION("should offer the made thing first among what may be delivered")
        {
            REQUIRE(f.script.Lines(1).front() == "a Gold Diamond Necklace");
        }

        SECTION("should offer making it beside stealing and buying it")
        {
            REQUIRE(f.script.asked[3].templateName == kObtain);
            REQUIRE(f.script.Offered(3) == std::vector<std::string>{"steal", "buy", "set"});
            REQUIRE(f.script.Lines(3)[2] == "Someone makes a Gold Diamond Necklace at a jeweler's bench.");
        }

        SECTION("should plan the making to supply the delivery")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "set");
            REQUIRE(FormOf(out.plan[0], "item") == kNecklace);
            REQUIRE(out.plan[1].verb == "deliver");
            REQUIRE(PlotPlanning::Serves(out.plan[0], out.plan[1]));
        }
    }

    SECTION("when a denunciation is prepared by hiding a poison among the person's belongings")
    {
        // goal denounce -> Hulda (c1). Prepare p1, the planting; its item is
        // the poison the finish names (c1), which can be stolen or concocted.
        f.finish = "Denounce Hulda to Laila for keeping a poison of lingering damage";
        f.script.answers = {"denounce", "c1", "p1", "c1", "concoct", "ready"};
        const auto out = f.Run();

        SECTION("should concoct it, plant it, then denounce")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 3);
            REQUIRE(out.plan[0].verb == "concoct");
            REQUIRE(FormOf(out.plan[0], "item") == kPoison);
            REQUIRE(out.plan[1].verb == "plant");
            REQUIRE(PlotPlanning::Serves(out.plan[0], out.plan[1]));
            REQUIRE(out.plan[2].verb == "denounce");
        }
    }

    SECTION("when the only jeweler is somebody the mastermind cannot reach")
    {
        f.population.members[1].crafts = 0;
        crafts(kJarl, PlotRecipes::Craft::Set);
        f.script.answers = {"deliver", "c1", "c1", "buy"};
        (void)f.Run();

        SECTION("should not offer the necklace at all")
        {
            const auto lines = f.script.Lines(1);
            REQUIRE(std::find(lines.begin(), lines.end(), "a Gold Diamond Necklace") == lines.end());
        }
    }
}

TEST_CASE("PlotPlanning never repeats a step", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the model answers the first option every time")
    {
        // goal steal -> ruby -> from Hulda; then p1, p1, ... for as long as
        // anything is offered.
        f.script.answers = {"steal", "c1", "c1", "p1", "p1"};
        const auto out = f.Run();

        SECTION("should stop once there is nothing new left to offer")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 3);
            REQUIRE(f.script.answers.empty());
        }

        SECTION("should hold no two steps alike")
        {
            for (std::size_t i = 0; i < out.plan.size(); ++i) {
                for (std::size_t j = i + 1; j < out.plan.size(); ++j) {
                    REQUIRE_FALSE(PlotPlanning::SameStep(out.plan[i], out.plan[j]));
                }
            }
        }

        SECTION("should read as a sequence: watch, ask around, then steal")
        {
            // Both prepare the theft, so each goes just before it in turn.
            REQUIRE(out.plan[0].verb == "watch");
            REQUIRE(out.plan[1].verb == "ask_around");
            REQUIRE(out.plan[2].verb == "steal");
        }

        SECTION("should stop offering a preparation once it is planned")
        {
            const auto second = f.script.Lines(4);
            REQUIRE(
                std::count(second.begin(), second.end(), "Before step 2: Someone watches Hulda, learning their habits.")
                == 0);
        }
    }

    SECTION("when re-planning a goal the plot has already carried out")
    {
        PlotModel::Step watched;
        watched.verb = "watch";
        watched.slots = {{"mastermind", kBoss, "Olfrid"}, {"mark", kTrader, "Hulda"}};
        watched.actor = kFriend;
        watched.outcome = PlotModel::StepOutcome::Succeeded;
        f.history = {watched};
        f.script.answers = {"watch"};
        const auto out = f.Run();

        SECTION("should leave out the filler that would make it the same step again")
        {
            REQUIRE(out.ok);
            REQUIRE(FormOf(out.plan.back(), "mark") == kFriend);
            // Idolaf was the only one left, and asking around about him the
            // only preparation, so nothing after the goal was asked.
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal});
        }
    }
}

TEST_CASE("PlotPlanning prepares only the goal and what supplies it", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when a preparation has been planned")
    {
        // goal steal -> ruby -> from Hulda; p1 is the watch of Hulda.
        f.script.answers = {"steal", "c1", "c1", "p1", "ready"};
        (void)f.Run();

        SECTION("should offer nothing that prepares the preparation")
        {
            const auto second = f.script.Lines(4);
            REQUIRE(std::count(second.begin(), second.end(), "Before step 1: Someone asks around [place] about Hulda.")
                    == 0);
        }

        SECTION("should still offer what prepares the goal")
        {
            const auto second = f.script.Lines(4);
            REQUIRE(std::count(second.begin(), second.end(), "Before step 2: Someone asks around [place] about Hulda.")
                    == 1);
        }
    }

    SECTION("when a step that gets an item has been planned")
    {
        // goal deliver -> ruby -> to Hulda; the theft from Idolaf is planned
        // unasked, and may be prepared like the goal.
        f.script.answers = {"deliver", "c1", "c1", "ready"};
        (void)f.Run();

        SECTION("should offer what prepares it")
        {
            REQUIRE(Contains(f.script.Lines(3), "Before step 1: Someone watches Idolaf, learning their habits."));
        }
    }
}

TEST_CASE("PlotPlanning keeps the scheme's people out of open slots", "[PlotPlanning][engine]")
{
    Fixture f;
    // Hulda, whom the scheme names, and Laila both have standing somewhere,
    // so either could be bribed.
    constexpr RE::FormID kGuild = 0x0000C001;
    for (auto& member : f.population.members) {
        if (member.npc == kTrader || member.npc == kJarl) {
            member.factions.push_back({kGuild, 0.5, true});
        }
    }

    SECTION("when a bribe before a theft from Idolaf is chosen")
    {
        // goal steal -> the ruby -> from Idolaf (c2; Hulda is named, first).
        // p3 is the open bribe.
        f.script.answers = {"steal", "c1", "c2", "p3", "ready"};
        const auto out = f.Run();

        SECTION("should never offer to bribe the person the scheme names")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan[0].verb == "bribe");
            REQUIRE(FormOf(out.plan[0], "mark") == kJarl);
            // Laila alone was left, so the bribe's slot was not asked.
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot, kSlot, kPrepare, kPrepare});
        }
    }
}

TEST_CASE("PlotPlanning ends the preparing", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the plan reaches its cap")
    {
        f.script.answers = {"steal", "c1", "c1", "p1"};
        const auto out = f.Run(2);

        SECTION("should stop asking")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(f.script.asked.size() == 4);
        }
    }

    SECTION("when the model answers ready once it may")
    {
        f.script.answers = {"steal", "c1", "c1", "p1", "ready"};
        const auto out = f.Run();

        SECTION("should not offer ready while the plan is the goal alone")
        {
            REQUIRE_FALSE(Contains(f.script.Offered(3), PlotPlanning::kReady));
            REQUIRE(Contains(f.script.Offered(4), PlotPlanning::kReady));
        }

        SECTION("should end the plan there")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(f.script.answers.empty());
        }
    }

    SECTION("when a preparation is answered with a key that was not offered")
    {
        f.script.answers = {"steal", "c1", "c1", "p9"};
        const auto out = f.Run();

        SECTION("should keep the plan as it stands rather than reject it")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 1);
            REQUIRE(out.plan[0].verb == "steal");
        }
    }

    SECTION("when a slot has one candidate")
    {
        // buy's seller is somebody who trades: only Hulda.
        f.script.answers = {"buy", "c2"};
        const auto out = f.Run();

        SECTION("should fill it without asking")
        {
            REQUIRE(out.ok);
            REQUIRE(FormOf(out.plan.back(), "seller") == kTrader);
            REQUIRE(FormOf(out.plan.back(), "item") == kSkooma);
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot});
        }
    }
}

TEST_CASE("PlotBirth::SchemeOpensWithVerb", "[PlotBirth][engine]")
{
    using NarrativeEngine::PlotBirth::SchemeOpensWithVerb;

    SECTION("when the scheme is an instruction")
    {
        SECTION("should accept it")
        {
            REQUIRE(SchemeOpensWithVerb("Spread a rumor that Hulda is a secret heavy drinker."));
            REQUIRE(SchemeOpensWithVerb("Steal Pantea Ateia's personal songbook"));
            REQUIRE(SchemeOpensWithVerb("  Bribe the dockmaster"));
        }
    }

    SECTION("when the scheme is narration or opens with a subject")
    {
        SECTION("should refuse it")
        {
            // From an audited run, word for word.
            REQUIRE_FALSE(SchemeOpensWithVerb("He is secretly funding a small excavation beneath the city ruins."));
            REQUIRE_FALSE(SchemeOpensWithVerb("The College caused the Great Collapse"));
            REQUIRE_FALSE(SchemeOpensWithVerb("Korir plans to expose the College"));
            REQUIRE_FALSE(SchemeOpensWithVerb("To ruin the Gray-Manes"));
            REQUIRE_FALSE(SchemeOpensWithVerb(""));
        }
    }
}

TEST_CASE("PlotBirth::ParseAmbition reads the goal", "[PlotBirth][engine]")
{
    using NarrativeEngine::PlotBirth::ParseAmbition;
    using NarrativeEngine::PlotBirth::TextLimits;

    const auto parse = [](const std::string& goal) {
        nlohmann::json j;
        j["reasoning"] = "x";
        j["ambition"] = "He means to own Whiterun.";
        j["scheme"] = "expose Hulda's skooma trade";
        j["finish"] = "denounce Hulda to Balgruuf for selling skooma";
        if (!goal.empty()) {
            j["goal"] = goal;
            j["roles"] = RolesFor(goal);
        }
        return ParseAmbition(j.dump(), TextLimits{});
    };

    SECTION("when the goal is a verb the manifest has")
    {
        const auto out = parse("denounce");

        SECTION("should keep it, and the scheme and finish apart")
        {
            REQUIRE(out.ok);
            REQUIRE(out.goal == "denounce");
            REQUIRE(out.scheme == "expose Hulda's skooma trade");
            REQUIRE(out.finish == "denounce Hulda to Balgruuf for selling skooma");
        }
    }

    SECTION("when the goal is not a verb the manifest has, or is missing")
    {
        SECTION("should refuse the answer, saying why")
        {
            const auto unknown = parse("document");
            REQUIRE_FALSE(unknown.ok);
            REQUIRE(unknown.rejection.find("document") != std::string::npos);
            REQUIRE(unknown.scheme.empty());
            REQUIRE_FALSE(parse("").ok);
        }
    }
}

TEST_CASE("PlotStepParse::NamesThePlayer", "[PlotBirth][engine]")
{
    using NarrativeEngine::PlotStepParse::NamesThePlayer;

    SECTION("when the text names the player's character")
    {
        SECTION("should say so, whole word and possessive alike")
        {
            REQUIRE(NamesThePlayer("denounce Faralda to Varian", "Varian"));
            REQUIRE(NamesThePlayer("steal Varian's journal", "Varian"));
            REQUIRE(NamesThePlayer("win over Brand-Shei for Lydia Stormblade", "Lydia Stormblade"));
        }
    }

    SECTION("when the text uses the title")
    {
        SECTION("should say so whatever the player is called")
        {
            REQUIRE(NamesThePlayer("rob the Dragonborn's house", "Varian"));
            REQUIRE(NamesThePlayer("ruin the dragonborn"));
        }
    }

    SECTION("when the name is only part of a word, in lowercase, or too short to be a name")
    {
        SECTION("should not count it")
        {
            REQUIRE_FALSE(NamesThePlayer("settle the variance in the ledgers", "Varian"));
            REQUIRE_FALSE(NamesThePlayer("denounce Faralda to Savos Aren", "Varian"));
            REQUIRE_FALSE(NamesThePlayer("a varian plot", "Varian"));
            REQUIRE_FALSE(NamesThePlayer("steal Al's ledger", "Al"));
        }
    }
}

TEST_CASE("PlotBirth::ParseAmbition refuses the player by name", "[PlotBirth][engine]")
{
    using NarrativeEngine::PlotBirth::ParseAmbition;
    using NarrativeEngine::PlotBirth::TextLimits;

    nlohmann::json j;
    j["reasoning"] = "x";
    j["ambition"] = "She means to run the College.";
    j["scheme"] = "bring Faralda down for abusing her authority";
    j["goal"] = "denounce";
    j["roles"] = RolesFor("denounce");
    j["finish"] = "denounce Faralda to Varian for abusing her authority";

    SECTION("when the finish names the player")
    {
        const auto out = ParseAmbition(j.dump(), TextLimits{}, "Varian");

        SECTION("should refuse it, saying why")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.rejection.find("player") != std::string::npos);
        }
    }

    SECTION("when the player is somebody else")
    {
        SECTION("should accept it")
        {
            REQUIRE(ParseAmbition(j.dump(), TextLimits{}, "Lydia").ok);
        }
    }
}

TEST_CASE("PlotBirth::ParseAmbition reads the finish", "[PlotBirth][engine]")
{
    using NarrativeEngine::PlotBirth::ParseAmbition;
    using NarrativeEngine::PlotBirth::TextLimits;

    nlohmann::json j;
    j["reasoning"] = "x";
    j["ambition"] = "He means to own Whiterun.";
    j["scheme"] = "expose Hulda's skooma trade";
    j["goal"] = "denounce";
    j["roles"] = RolesFor("denounce");

    SECTION("when the finish is missing")
    {
        SECTION("should refuse the answer")
        {
            REQUIRE_FALSE(ParseAmbition(j.dump(), TextLimits{}).ok);
        }
    }

    SECTION("when the finish writes its verb as the id from the list")
    {
        j["goal"] = "spread_rumors";
        j["roles"] = RolesFor("spread_rumors");
        j["finish"] = "spread_rumors about Hulda around Whiterun";

        SECTION("should write the id as words rather than refuse it")
        {
            const auto out = ParseAmbition(j.dump(), TextLimits{});
            REQUIRE(out.ok);
            REQUIRE(out.finish == "spread rumors about Hulda around Whiterun");
            REQUIRE(out.goal == "spread_rumors");
        }
    }

    SECTION("when the goal only prepares for a later step")
    {
        j["goal"] = "plant";
        j["roles"] = RolesFor("plant");
        j["finish"] = "hide a ruby among Hulda's belongings";
        const auto out = ParseAmbition(j.dump(), TextLimits{});

        SECTION("should refuse it, saying it cannot end a scheme")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.rejection.find("cannot end a scheme") != std::string::npos);
        }
    }

    SECTION("when the finish opens with a name rather than a verb")
    {
        j["finish"] = "Hulda is denounced to Balgruuf";

        SECTION("should refuse the answer, saying why")
        {
            const auto out = ParseAmbition(j.dump(), TextLimits{});
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.rejection.find("finish") != std::string::npos);
        }
    }
}

TEST_CASE("PlotBirth::ParseAmbition reads the roles", "[PlotBirth][engine]")
{
    using NarrativeEngine::PlotBirth::ParseAmbition;
    using NarrativeEngine::PlotBirth::TextLimits;

    nlohmann::json j;
    j["reasoning"] = "x";
    j["ambition"] = "He means to own Whiterun.";
    j["scheme"] = "expose Hulda's skooma trade";
    j["goal"] = "denounce";
    j["finish"] = "denounce Hulda to Balgruuf for selling skooma";

    SECTION("when a role of the goal is not named")
    {
        j["roles"] = {{"mark", "Hulda"}};
        const auto out = ParseAmbition(j.dump(), TextLimits{});

        SECTION("should refuse the answer, saying which role and what it is for")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.rejection.find("authority (whom they are denounced to)") != std::string::npos);
        }
    }

    SECTION("when a role names the player")
    {
        j["roles"] = {{"mark", "Varian"}, {"authority", "Balgruuf"}};

        SECTION("should refuse the answer")
        {
            REQUIRE_FALSE(ParseAmbition(j.dump(), TextLimits{}, "Varian").ok);
        }
    }

    SECTION("when every role is named and resolves")
    {
        j["roles"] = {{"authority", "Balgruuf the Greater"}, {"mark", "Hulda"}};
        std::vector<NarrativeEngine::PlotModel::SlotValue> asked;
        const auto out =
            ParseAmbition(j.dump(), TextLimits{}, {}, [&asked](const auto&, const auto& named, auto& resolved) {
                asked = named;
                resolved = {{"mark", kTrader, "Hulda"}, {"authority", kJarl, "Laila"}};
                return std::string{};
            });

        SECTION("should hand the resolver each role in the goal's slot order, and keep what it resolved")
        {
            REQUIRE(out.ok);
            REQUIRE(asked.size() == 2);
            REQUIRE(asked[0].name == "mark");
            REQUIRE(asked[0].text == "Hulda");
            REQUIRE(asked[1].text == "Balgruuf the Greater");
            REQUIRE(out.roles.size() == 2);
            REQUIRE(out.roles[1].form == kJarl);
        }
    }

    SECTION("when the roles do not fit")
    {
        j["roles"] = {{"mark", "Hulda"}, {"authority", "Balgruuf"}};
        const auto out = ParseAmbition(j.dump(), TextLimits{}, {}, [](const auto&, const auto&, auto&) {
            return std::string{"Balgruuf cannot be the authority"};
        });

        SECTION("should refuse the answer with the resolver's reason")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.rejection == "the roles do not fit: Balgruuf cannot be the authority");
            REQUIRE(out.roles.empty());
        }
    }
}

TEST_CASE("PlotPlanning::PickAgent", "[PlotPlanning][engine]")
{
    Script script;
    const auto ask = [&script](std::string_view n, const nlohmann::json& c) { return script(n, c); };

    PlotModel::Plot plot;
    plot.mastermindName = "Olfrid";
    plot.scheme = "Ruin Hulda's trade.";
    PlotModel::Step step;
    step.verb = "watch";
    step.slots.push_back({"mark", kTrader, "Hulda"});

    const std::vector<PlotPlanning::AgentOption> two{{kFriend, "Idolaf (someone they know)"},
                                                     {kBoss, "Olfrid themselves"}};

    SECTION("when only one person may be asked")
    {
        std::string error;
        const auto agent = PlotPlanning::PickAgent(plot, step, {two.front()}, ask, error);

        SECTION("should take them without asking")
        {
            REQUIRE(agent == kFriend);
            REQUIRE(script.asked.empty());
            REQUIRE(error.empty());
        }
    }

    SECTION("when several may be asked")
    {
        script.answers = {"c2"};
        std::string error;
        const auto agent = PlotPlanning::PickAgent(plot, step, two, ask, error);

        SECTION("should ask, with the agent shown open in the step")
        {
            REQUIRE(agent == kBoss);
            REQUIRE(script.asked.size() == 1);
            REQUIRE(script.asked[0].ctx["slot"] == "agent");
            REQUIRE(script.asked[0].ctx["step"] == "[agent] watches Hulda, learning their habits.");
        }
    }

    SECTION("when the decision fails")
    {
        script.failWith = "request_failed";
        std::string error;
        const auto agent = PlotPlanning::PickAgent(plot, step, two, ask, error);

        SECTION("should take the ladder's first rather than stall, and say why")
        {
            REQUIRE(agent == kFriend);
            REQUIRE(error == "request_failed");
        }
    }

    SECTION("when the answer was not offered")
    {
        script.answers = {"c7"};
        std::string error;
        const auto agent = PlotPlanning::PickAgent(plot, step, two, ask, error);

        SECTION("should take the ladder's first")
        {
            REQUIRE(agent == kFriend);
            REQUIRE_FALSE(error.empty());
        }
    }
}

TEST_CASE("PlotPlanning::ItemHolder", "[PlotPlanning][engine]")
{
    PlotModel::Plot plot;
    PlotModel::Step ruby;
    ruby.verb = "steal";
    ruby.slots = {{"item", kRuby, "a ruby"}};
    ruby.actor = kFriend;
    PlotModel::Step skooma;
    skooma.verb = "buy";
    skooma.slots = {{"item", kSkooma, "a bottle of skooma"}};
    skooma.actor = kTrader;
    skooma.outcome = PlotModel::StepOutcome::Succeeded;

    PlotModel::Step deliverRuby;
    deliverRuby.verb = "deliver";
    deliverRuby.slots = {{"item", kRuby, "a ruby"}};

    SECTION("when an earlier step stole the item, and a later one got something else")
    {
        ruby.outcome = PlotModel::StepOutcome::Succeeded;
        plot.history = {ruby, skooma};

        SECTION("should be whoever got that very item, not whoever got something last")
        {
            REQUIRE(PlotPlanning::ItemHolder(plot, deliverRuby) == kFriend);
        }
    }

    SECTION("when the theft failed")
    {
        ruby.outcome = PlotModel::StepOutcome::FailedCaught;
        plot.history = {ruby, skooma};

        SECTION("should be nobody, since nobody has it")
        {
            REQUIRE(PlotPlanning::ItemHolder(plot, deliverRuby) == 0);
        }
    }
}

namespace
{
    PlotModel::Step Planned(std::string_view verb, std::vector<PlotModel::SlotValue> slots)
    {
        PlotModel::Step step;
        step.verb = std::string{verb};
        step.slots = {{"mastermind", kBoss, "Olfrid"}};
        for (auto& value : slots) {
            step.slots.push_back(std::move(value));
        }
        return step;
    }

    // Thonar's plot from Step 9's fourth attempt, in Olfrid's province:
    // watch Idolaf, steal the ruby from him, watch Hulda, plant the ruby on
    // Hulda. `failedAt` says which step was caught; it sits at the cursor and
    // is the last entry in the history.
    PlotModel::Plot Thonar(std::size_t failedAt)
    {
        PlotModel::Plot plot;
        plot.mastermind = kBoss;
        plot.mastermindName = "Olfrid";
        plot.scheme = "Ruin Hulda's trade.";
        plot.finish = "plant a ruby among Hulda's belongings";
        plot.plan = {Planned("watch", {{"mark", kFriend, "Idolaf"}}),
                     Planned("steal", {{"item", kRuby, "a ruby"}, {"mark", kFriend, "Idolaf"}}),
                     Planned("watch", {{"mark", kTrader, "Hulda"}}),
                     Planned("plant", {{"item", kRuby, "a ruby"}, {"mark", kTrader, "Hulda"}})};
        for (std::size_t i = 0; i < failedAt; ++i) {
            plot.plan[i].outcome = PlotModel::StepOutcome::Succeeded;
            plot.history.push_back(plot.plan[i]);
        }
        auto failed = plot.plan[failedAt];
        failed.actor = kFriend;
        failed.outcome = PlotModel::StepOutcome::FailedCaught;
        failed.state = PlotModel::StepState::Failed;
        plot.history.push_back(failed);
        plot.cursor = failedAt;
        return plot;
    }

    PlotPlanning::Outcome OneStep(std::string_view verb)
    {
        PlotPlanning::Outcome out;
        out.plan.push_back(Planned(verb, {{"mark", kTrader, "Hulda"}}));
        out.ok = true;
        return out;
    }

    bool HasWay(const std::vector<PlotModel::Abandoned>& ways, std::string_view verb, RE::FormID target)
    {
        return std::any_of(ways.begin(), ways.end(), [&](const PlotModel::Abandoned& a) {
            return a.verb == verb && a.target == target;
        });
    }
} // namespace

TEST_CASE("PlotPlanning::RouteOf", "[PlotPlanning][engine]")
{
    SECTION("when a preparation fails")
    {
        const auto route = PlotPlanning::RouteOf(Thonar(0));

        SECTION("should abandon it and the theft it was preparing")
        {
            REQUIRE(route.size() == 2);
            REQUIRE(HasWay(route, "watch", kFriend));
            REQUIRE(HasWay(route, "steal", kFriend));
            REQUIRE(route.front().targetName == "Idolaf");
        }

        SECTION("should keep the finish, and a preparation of it that the failure did not touch")
        {
            REQUIRE_FALSE(HasWay(route, "plant", kTrader));
            REQUIRE_FALSE(HasWay(route, "watch", kTrader));
        }
    }

    SECTION("when the finish itself fails")
    {
        SECTION("should fail the scheme rather than look for a route around it")
        {
            REQUIRE(PlotPlanning::FinishFailed(Thonar(3)));
        }
    }

    SECTION("when a step before the finish fails")
    {
        SECTION("should leave the scheme to adapt")
        {
            REQUIRE_FALSE(PlotPlanning::FinishFailed(Thonar(0)));
            REQUIRE_FALSE(PlotPlanning::FinishFailed(Thonar(2)));
        }
    }

    SECTION("when asking what serves what")
    {
        const auto plot = Thonar(0);

        SECTION("should follow bound preparations and items, and nothing else")
        {
            REQUIRE(PlotPlanning::Serves(plot.plan[0], plot.plan[1]));       // watch Idolaf -> steal from Idolaf
            REQUIRE(PlotPlanning::Serves(plot.plan[1], plot.plan[3]));       // the ruby -> planting it
            REQUIRE(PlotPlanning::Serves(plot.plan[2], plot.plan[3]));       // watch Hulda -> plant on Hulda
            REQUIRE_FALSE(PlotPlanning::Serves(plot.plan[0], plot.plan[3])); // watch Idolaf, plant on Hulda
        }
    }
}

TEST_CASE("PlotPlanning::Adapt", "[PlotPlanning][engine]")
{
    Script script;
    const auto ask = [&script](std::string_view n, const nlohmann::json& c) { return script(n, c); };
    int replans = 0;
    std::vector<PlotModel::Abandoned> seenByReplan;
    const auto replan = [&](const PlotModel::Plot& p) {
        ++replans;
        seenByReplan = p.abandoned;
        return OneStep("ask_around");
    };

    SECTION("when a step fails")
    {
        auto plot = Thonar(0);
        script.answers = {"concede"};
        (void)PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should offer another way or giving up, and never the same step again")
        {
            REQUIRE(script.Offered(0) == std::vector<std::string>{PlotPlanning::kSubstitute, PlotPlanning::kConcede});
        }
    }

    SECTION("when the schemer finds another way")
    {
        auto plot = Thonar(0);
        script.answers = {"substitute"};
        const auto a = PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should abandon the failed route before planning, so the plan cannot reach for it")
        {
            REQUIRE(replans == 1);
            REQUIRE(HasWay(seenByReplan, "watch", kFriend));
            REQUIRE(HasWay(seenByReplan, "steal", kFriend));
            REQUIRE(HasWay(plot.abandoned, "steal", kFriend));
            REQUIRE(a.abandoned.size() == 2);
        }

        SECTION("should replace the plan from the failed position on, and nothing behind it")
        {
            REQUIRE(a.revision == PlotPlanning::Revision::Substituted);
            REQUIRE(a.newTail == 1);
            REQUIRE(plot.plan.size() == 1);
            REQUIRE(plot.plan[0].verb == "ask_around");
            REQUIRE(plot.history.size() == 1);
            REQUIRE(plot.adaptations == 1);
        }
    }

    SECTION("when a way already abandoned fails to be needed again")
    {
        auto plot = Thonar(0);
        plot.abandoned.push_back({"watch", kFriend, "Idolaf"});
        script.answers = {"substitute"};
        const auto a = PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should record each way once")
        {
            REQUIRE(plot.abandoned.size() == 2);
            REQUIRE(a.abandoned.size() == 1);
        }
    }

    SECTION("when another way cannot be planned")
    {
        auto plot = Thonar(0);
        script.answers = {"substitute"};
        const auto a = PlotPlanning::Adapt(plot, 3, ask, [](const PlotModel::Plot&) {
            PlotPlanning::Outcome nothing;
            nothing.error = "request_failed";
            return nothing;
        });

        SECTION("should concede rather than leave the plot stalled")
        {
            REQUIRE(a.revision == PlotPlanning::Revision::Conceded);
            REQUIRE(a.reason.find("request_failed") != std::string::npos);
        }
    }

    SECTION("when the schemer gives up")
    {
        auto plot = Thonar(0);
        script.answers = {"concede"};
        const auto a = PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should concede, abandoning nothing")
        {
            REQUIRE(a.revision == PlotPlanning::Revision::Conceded);
            REQUIRE(plot.abandoned.empty());
            REQUIRE(replans == 0);
        }
    }

    SECTION("when the answer was the old retry")
    {
        auto plot = Thonar(0);
        script.answers = {"retry"};

        SECTION("should concede, since it was not offered")
        {
            REQUIRE(PlotPlanning::Adapt(plot, 3, ask, replan).revision == PlotPlanning::Revision::Conceded);
        }
    }

    SECTION("when the decision fails")
    {
        auto plot = Thonar(0);
        script.failWith = "request_failed";

        SECTION("should concede")
        {
            REQUIRE(PlotPlanning::Adapt(plot, 3, ask, replan).revision == PlotPlanning::Revision::Conceded);
        }
    }

    SECTION("when the plot has spent its attempts")
    {
        auto plot = Thonar(0);
        plot.adaptations = 3;
        const auto a = PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should end it without asking")
        {
            REQUIRE(a.revision == PlotPlanning::Revision::CapHit);
            REQUIRE(script.asked.empty());
            REQUIRE(plot.adaptations == 3);
        }
    }
}

TEST_CASE("PlotPlanning plans around abandoned ways", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when stealing the item from the only person who has it was abandoned")
    {
        // deliver the ruby to Idolaf: Hulda could be robbed of it or sell it,
        // and robbing her has been given up on.
        f.abandoned = {{"steal", kTrader, "Hulda"}};
        f.script.answers = {"deliver", "c1", "c2", "ready"};
        const auto out = f.Run();

        SECTION("should get it the other way, without asking which")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan[0].verb == "buy");
            REQUIRE(FormOf(out.plan[0], "seller") == kTrader);
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot, kSlot, kPrepare});
        }
    }

    SECTION("when the goal's verb against the person the scheme names was abandoned")
    {
        f.abandoned = {{"watch", kTrader, "Hulda"}};
        f.script.answers = {"watch"};
        const auto out = f.Run();

        SECTION("should leave that person off the goal's list")
        {
            REQUIRE(out.ok);
            REQUIRE(FormOf(out.plan.back(), "mark") == kFriend);
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal});
        }
    }

    SECTION("when a preparation was abandoned")
    {
        f.abandoned = {{"watch", kTrader, "Hulda"}};
        f.script.answers = {"steal", "c1", "c1"};
        const auto out = f.Run();

        SECTION("should not offer it, leaving the other preparation to be planned unasked")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "ask_around");
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot, kSlot});
        }
    }
}
