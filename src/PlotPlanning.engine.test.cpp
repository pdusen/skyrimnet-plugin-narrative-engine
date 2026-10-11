#include <PlotPlanning.h>

#include <PlotBirth.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

// Tests for planning a scheme by decision.
//
// The decision call is scripted: each test says what the model answers, in
// order, and the suite asserts what the plan can and cannot become. What
// matters is that nothing reaches a plan that was not on a list the planner
// built -- not a verb, not a person, not an item -- and that every way the
// loop can end, ends it.

namespace
{
    namespace PlotPlanning = NarrativeEngine::PlotPlanning;
    namespace PlotCasting = NarrativeEngine::PlotCasting;
    namespace PlotMenus = NarrativeEngine::PlotMenus;
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;

    constexpr RE::FormID kWhiterun = 0x0000A001;
    constexpr RE::FormID kHold = 0x0000B001;
    constexpr RE::FormID kBoss = 0x00010001;
    constexpr RE::FormID kFriend = 0x00010002;
    constexpr RE::FormID kTrader = 0x00010003;
    constexpr RE::FormID kRuby = 0x0000D001;
    constexpr RE::FormID kSkooma = 0x0000D002;

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
        return p;
    }

    PlotMenus::World World()
    {
        PlotMenus::World w;
        w.settlements = {{kWhiterun, "Whiterun"}};
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
            const bool verb = name == PlotPlanning::kVerbTemplate;
            r.answers.emplace_back(verb ? PlotPlanning::kVerbQuestion : PlotPlanning::kSlotQuestion, a);
            r.ok = true;
            return r;
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
    };

    struct Fixture
    {
        PlotCasting::Population population = Province();
        PlotMenus::World world = World();
        Script script;

        PlotPlanning::Outcome Run(std::size_t maxSteps = 6)
        {
            PlotPlanning::Input input{population, world};
            input.mastermind = kBoss;
            input.mastermindName = "Olfrid";
            input.ambition = "He means to own Whiterun.";
            input.scheme = "Ruin Hulda's trade.";
            input.maxSteps = maxSteps;
            return PlotPlanning::Plan(input,
                                      [this](std::string_view n, const nlohmann::json& c) { return script(n, c); });
        }
    };

    bool Contains(const std::vector<std::string>& keys, std::string_view key)
    {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    }
} // namespace

TEST_CASE("PlotPlanning lays out a plan from offered keys", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when the model picks a verb and then ends the plan")
    {
        // watch -> its mark has two candidates (friend, trader), so it is asked.
        f.script.answers = {"watch", "c2", "done"};
        const auto out = f.Run();

        SECTION("should produce one step of that verb, filled from the list")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 1);
            const auto& step = out.plan.front();
            REQUIRE(step.verb == "watch");
            REQUIRE(step.type == PlotModel::StepType::Surveil);
            REQUIRE(step.FindSlot("mark")->form == kTrader);
            REQUIRE(step.FindSlot("mastermind")->form == kBoss);
        }

        SECTION("should make the mark the step's target, for the resolution model")
        {
            REQUIRE(out.plan.front().target == kTrader);
            REQUIRE(out.plan.front().targetName == "Hulda");
        }

        SECTION("should leave the agent for dispatch")
        {
            REQUIRE(out.plan.front().actor == 0);
            REQUIRE(PlotModel::Label(out.plan.front()) == "Someone watches Hulda, learning their habits.");
        }

        SECTION("should ask the verb question, then the slot, then the verb question again")
        {
            REQUIRE(f.script.asked.size() == 3);
            REQUIRE(f.script.asked[0].templateName == PlotPlanning::kVerbTemplate);
            REQUIRE(f.script.asked[1].templateName == PlotPlanning::kSlotTemplate);
            REQUIRE(f.script.asked[1].ctx["slot"] == "mark");
            REQUIRE(f.script.asked[1].ctx["step"] == "Someone watches [mark], learning their habits.");
        }

        SECTION("should show the second verb question the plan so far")
        {
            const auto steps = f.script.asked[2].ctx["steps"];
            REQUIRE(steps.size() == 1);
            REQUIRE(steps[0] == "1. Someone watches Hulda, learning their habits.");
        }
    }

    SECTION("when the model answers a verb that was not offered")
    {
        // deliver needs an item obtained earlier; nothing has obtained one.
        f.script.answers = {"deliver"};
        const auto out = f.Run();

        SECTION("should reject the plan rather than run it")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.plan.empty());
            REQUIRE(out.rejection.find("deliver") != std::string::npos);
        }
    }

    SECTION("when the model answers a slot key that was not offered")
    {
        f.script.answers = {"watch", "c9"};
        const auto out = f.Run();

        SECTION("should reject the plan")
        {
            REQUIRE_FALSE(out.ok);
            REQUIRE(out.plan.empty());
        }
    }
}

TEST_CASE("PlotPlanning gates and ends the plan", "[PlotPlanning][engine]")
{
    Fixture f;

    SECTION("when nothing has obtained an item yet")
    {
        f.script.answers = {"steal", "c1", "c2", "deliver", "c1", "done"};
        const auto out = f.Run();

        SECTION("should not offer deliver or plant at the first position")
        {
            const auto first = f.script.Offered(0);
            REQUIRE_FALSE(Contains(first, "deliver"));
            REQUIRE_FALSE(Contains(first, "plant"));
        }

        SECTION("should offer them once a step has obtained something")
        {
            REQUIRE(Contains(f.script.Offered(3), "deliver"));
            REQUIRE(Contains(f.script.Offered(3), "plant"));
        }

        SECTION("should bind the delivered item to the stolen one without asking")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 2);
            REQUIRE(out.plan[0].FindSlot("item")->form == kRuby);
            REQUIRE(out.plan[1].FindSlot("item")->form == kRuby);
            // steal: item, mark asked; deliver: recipient asked, item bound.
            const auto slotQuestions = std::count_if(f.script.asked.begin(), f.script.asked.end(), [](const auto& a) {
                return a.templateName == PlotPlanning::kSlotTemplate;
            });
            REQUIRE(slotQuestions == 3);
        }
    }

    SECTION("when the plan is at its first position")
    {
        f.script.answers = {"watch", "c1", "done"};
        (void)f.Run();

        SECTION("should not offer done, since one step is not yet a plan")
        {
            REQUIRE_FALSE(Contains(f.script.Offered(0), PlotPlanning::kDone));
            REQUIRE(Contains(f.script.Offered(2), PlotPlanning::kDone));
        }
    }

    SECTION("when the model never answers done")
    {
        f.script.answers = {"watch", "c1", "watch", "c1", "watch", "c1"};
        const auto out = f.Run(3);

        SECTION("should stop at the position cap")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan.size() == 3);
        }
    }

    SECTION("when a slot has one candidate")
    {
        // buy's seller is somebody who trades: only Hulda.
        f.script.answers = {"buy", "c2", "done"};
        const auto out = f.Run();

        SECTION("should fill it without asking")
        {
            REQUIRE(out.ok);
            REQUIRE(out.plan[0].FindSlot("seller")->form == kTrader);
            REQUIRE(out.plan[0].FindSlot("item")->form == kSkooma);
            REQUIRE(f.script.asked.size() == 3); // verb, item, verb
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
