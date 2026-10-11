#include <PlotPlanning.h>

#include <PlotBirth.h>

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
        jarl.authority = true;
        p.members.push_back(jarl);
        return p;
    }

    PlotMenus::World World()
    {
        PlotMenus::World w;
        w.settlements = {{kWhiterun, "Whiterun"}, {kRiften, "Riften"}};
        w.items = {{kRuby, "a ruby", {}}, {kSkooma, "a bottle of skooma", {}}};
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
        Script script;

        PlotPlanning::Outcome Run(std::size_t maxSteps = PlotPlanning::kPlanCap)
        {
            PlotPlanning::Input input{population, world};
            input.goal = goal;
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

    const std::string kGoal = PlotPlanning::kGoalTemplate;
    const std::string kSlot = PlotPlanning::kSlotTemplate;
    const std::string kPrepare = PlotPlanning::kPrepareTemplate;
    const std::string kObtain = PlotPlanning::kObtainTemplate;
} // namespace

TEST_CASE("PlotPlanning asks for the goal first", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the goal is a watch and one preparation is taken")
    {
        // goal watch -> mark: Hulda (named, first) or Idolaf -> prepare p1:
        // ask around about Hulda, whose place is Whiterun alone. Then nothing
        // is left to offer, so planning stops on its own.
        f.script.answers = {"watch", "c1", "p1"};
        const auto out = f.Run();

        SECTION("should ask the goal, its slot, then what comes first")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot, kPrepare});
            REQUIRE(out.decisions == 3);
        }

        SECTION("should put the goal last and the preparation before it")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].verb == "ask_around");
            REQUIRE(out.plan[1].verb == "watch");
        }

        SECTION("should bind the preparation to the person its step is about, without asking")
        {
            REQUIRE(FormOf(out.plan[1], "mark") == kTrader);
            REQUIRE(FormOf(out.plan[0], "mark") == kTrader);
            REQUIRE(FormOf(out.plan[0], "place") == kWhiterun);
        }

        SECTION("should offer the preparation as a whole step, saying where it goes")
        {
            REQUIRE(f.script.Lines(2)
                    == std::vector<std::string>{"Before step 1: Someone asks around [place] about Hulda.",
                                                "Nothing more: the plan is ready to carry out."});
            REQUIRE(f.script.asked[2].ctx["plan"][0] == "1. Someone watches Hulda, learning their habits.");
        }

        SECTION("should fill the goal's slot knowing it is the goal")
        {
            REQUIRE(f.script.asked[1].ctx["step"] == "Someone watches [mark], learning their habits.");
            REQUIRE(f.script.asked[1].ctx["purpose"] == "This step accomplishes the scheme.");
        }

        SECTION("should leave the agent for dispatch")
        {
            REQUIRE(out.plan[1].actor == 0);
            REQUIRE(out.plan[1].FindSlot("mastermind")->form == kBoss);
            REQUIRE(out.plan[1].target == kTrader);
        }
    }

    SECTION("when the scheme was written to end in a goal")
    {
        f.goal = "watch";
        f.script.answers = {"c1", "p1"};
        const auto out = f.Run();

        SECTION("should plan it last without asking for it")
        {
            REQUIRE(f.script.Templates() == std::vector<std::string>{kSlot, kPrepare});
            REQUIRE(out.ok);
            REQUIRE(out.plan.back().verb == "watch");
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
            REQUIRE(f.script.Offered(1) == std::vector<std::string>{"c1", "c2"});
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

        SECTION("should read as a sequence: ask around, watch, then steal")
        {
            REQUIRE(out.plan[0].verb == "ask_around");
            REQUIRE(out.plan[1].verb == "watch");
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
        f.script.answers = {"watch", "ready"};
        const auto out = f.Run();

        SECTION("should leave out the filler that would make it the same step again")
        {
            REQUIRE(out.ok);
            REQUIRE(FormOf(out.plan.back(), "mark") == kFriend);
            // Idolaf was the only one left, so nobody was asked.
            REQUIRE(f.script.asked[1].templateName == kPrepare);
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

    SECTION("when the model answers ready")
    {
        f.script.answers = {"steal", "c1", "c1", "ready"};
        const auto out = f.Run();

        SECTION("should keep just the goal")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 1);
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
        f.script.answers = {"buy", "c2", "ready"};
        const auto out = f.Run();

        SECTION("should fill it without asking")
        {
            REQUIRE(out.ok);
            REQUIRE(FormOf(out.plan[0], "seller") == kTrader);
            REQUIRE(FormOf(out.plan[0], "item") == kSkooma);
            REQUIRE(f.script.Templates() == std::vector<std::string>{kGoal, kSlot, kPrepare});
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
        j["scheme"] = "denounce Hulda to the jarl for selling skooma";
        if (!goal.empty()) {
            j["goal"] = goal;
        }
        return ParseAmbition(j.dump(), TextLimits{});
    };

    SECTION("when the goal is a verb the manifest has")
    {
        const auto out = parse("denounce");

        SECTION("should keep it")
        {
            REQUIRE(out.ok);
            REQUIRE(out.goal == "denounce");
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
    // A plot whose second step just failed: one step done, the failed one
    // moved to history and still sitting at the cursor, one more planned.
    PlotModel::Plot AfterAFailure(std::string_view failedVerb, PlotModel::StepOutcome how)
    {
        PlotModel::Plot plot;
        plot.mastermind = kBoss;
        plot.mastermindName = "Olfrid";
        plot.scheme = "Ruin Hulda's trade.";

        PlotModel::Step done;
        done.verb = "steal";
        done.slots = {{"item", kRuby, "a ruby"}, {"mark", kTrader, "Hulda"}};
        done.actor = kFriend;
        done.outcome = PlotModel::StepOutcome::Succeeded;
        done.state = PlotModel::StepState::Succeeded;

        PlotModel::Step failed;
        failed.verb = std::string{failedVerb};
        failed.slots = {{"mark", kTrader, "Hulda"}};
        failed.actor = kFriend;
        failed.outcome = how;
        failed.state = PlotModel::StepState::Failed;
        failed.progress = 3.0f;
        failed.elapsed = 4;

        PlotModel::Step next;
        next.verb = "threaten";
        next.slots = {{"mark", kTrader, "Hulda"}};

        plot.plan = {done, failed, next};
        plot.cursor = 1;
        plot.history = {done, failed};
        return plot;
    }

    PlotPlanning::Outcome OneStep(std::string_view verb)
    {
        PlotPlanning::Outcome out;
        PlotModel::Step step;
        step.verb = std::string{verb};
        step.slots = {{"mark", kTrader, "Hulda"}};
        out.plan.push_back(step);
        out.ok = true;
        return out;
    }
} // namespace

TEST_CASE("PlotPlanning::Adapt", "[PlotPlanning][engine]")
{
    Script script;
    const auto ask = [&script](std::string_view n, const nlohmann::json& c) { return script(n, c); };
    int replans = 0;
    const auto replan = [&replans](const PlotModel::Plot&) {
        ++replans;
        return OneStep("ask_around");
    };

    SECTION("when the schemer retries")
    {
        auto plot = AfterAFailure("watch", PlotModel::StepOutcome::FailedCaught);
        script.answers = {"retry"};
        const auto a = PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should reset the step at the cursor and keep everything else")
        {
            REQUIRE(a.revision == PlotPlanning::Revision::Retried);
            REQUIRE(plot.plan.size() == 3);
            const auto& retried = plot.plan[1];
            REQUIRE(retried.verb == "watch");
            REQUIRE(retried.state == PlotModel::StepState::Planned);
            REQUIRE(retried.actor == 0);
            REQUIRE(retried.progress == 0.0f);
            REQUIRE(retried.elapsed == 0);
            REQUIRE(plot.plan[2].verb == "threaten");
            REQUIRE(plot.history.size() == 2);
            REQUIRE(plot.adaptations == 1);
        }

        SECTION("should keep the retry from whoever failed it")
        {
            const auto failed = PlotPlanning::FailedAgents(plot, plot.plan[1]);
            REQUIRE(failed.size() == 1);
            REQUIRE(failed.contains(kFriend));
        }
    }

    SECTION("when the failed step's agent is bound to the item it carries")
    {
        auto plot = AfterAFailure("deliver", PlotModel::StepOutcome::FailedTimeout);
        script.answers = {"concede"};
        (void)PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should not offer a retry, since nobody else holds the item")
        {
            const auto offered = script.Offered(0);
            REQUIRE_FALSE(Contains(offered, PlotPlanning::kRetry));
            REQUIRE(Contains(offered, PlotPlanning::kSubstitute));
            REQUIRE(Contains(offered, PlotPlanning::kConcede));
        }
    }

    SECTION("when the schemer tries another way")
    {
        auto plot = AfterAFailure("watch", PlotModel::StepOutcome::FailedCaught);
        script.answers = {"substitute"};
        const auto a = PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should replace the plan from the failed position on, and nothing behind it")
        {
            REQUIRE(a.revision == PlotPlanning::Revision::Substituted);
            REQUIRE(a.newTail == 1);
            REQUIRE(replans == 1);
            REQUIRE(plot.plan.size() == 2);
            REQUIRE(plot.plan[0].verb == "steal");
            REQUIRE(plot.plan[1].verb == "ask_around");
            REQUIRE(plot.history.size() == 2);
        }
    }

    SECTION("when another way cannot be planned")
    {
        auto plot = AfterAFailure("watch", PlotModel::StepOutcome::FailedCaught);
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
        auto plot = AfterAFailure("watch", PlotModel::StepOutcome::FailedCaught);
        script.answers = {"concede"};

        SECTION("should concede")
        {
            REQUIRE(PlotPlanning::Adapt(plot, 3, ask, replan).revision == PlotPlanning::Revision::Conceded);
        }
    }

    SECTION("when the decision fails")
    {
        auto plot = AfterAFailure("watch", PlotModel::StepOutcome::FailedCaught);
        script.failWith = "request_failed";

        SECTION("should concede")
        {
            REQUIRE(PlotPlanning::Adapt(plot, 3, ask, replan).revision == PlotPlanning::Revision::Conceded);
        }
    }

    SECTION("when the plot has spent its attempts")
    {
        auto plot = AfterAFailure("watch", PlotModel::StepOutcome::FailedCaught);
        plot.adaptations = 3;
        const auto a = PlotPlanning::Adapt(plot, 3, ask, replan);

        SECTION("should end it without asking")
        {
            REQUIRE(a.revision == PlotPlanning::Revision::CapHit);
            REQUIRE(script.asked.empty());
            REQUIRE(plot.adaptations == 3);
        }
    }

    SECTION("when a plot is fed nothing but retries")
    {
        auto plot = AfterAFailure("watch", PlotModel::StepOutcome::FailedCaught);
        script.answers = {"retry", "retry", "retry", "retry"};
        int rounds = 0;
        while (PlotPlanning::Adapt(plot, 3, ask, replan).revision == PlotPlanning::Revision::Retried) {
            ++rounds;
            REQUIRE(rounds <= 3);
        }

        SECTION("should stop at the cap, having retried once per attempt")
        {
            REQUIRE(rounds == 3);
            REQUIRE(plot.adaptations == 3);
        }
    }
}
