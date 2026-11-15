#include <PlotAwareness.h>

#include <PlotVerbs.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

// Tests for what a person in a plot is told of it in their own prompt.
//
// The plot is the one the prompt was designed around: Arcadia wants the
// Bannered Mare, sent Ysolda to ask about Hulda and lost that, has Saadia
// spreading talk now, and still has the deed to take.
//
// What a person is told is a record, which plot_awareness.prompt words; these
// cases check what each record says, and the render comparison checks how it
// reads. It matters to the word, because the reader is a model that will take
// each one at face value: telling an agent the scheme, or a mastermind that
// they sent somebody they did not, is a person in Skyrim acting on a thing
// that is not so.

namespace
{
    namespace PlotAwareness = NarrativeEngine::PlotAwareness;
    namespace PlotModel = NarrativeEngine::PlotModel;
    using NarrativeEngine::PlotState;
    using PlotModel::Role;
    using PlotModel::StepOutcome;
    using PlotModel::StepState;

    constexpr RE::FormID kBoss = 0x00010001;
    constexpr RE::FormID kYsolda = 0x00010002;
    constexpr RE::FormID kSaadia = 0x00010003;
    constexpr RE::FormID kHulda = 0x00010004;
    constexpr RE::FormID kStranger = 0x00010005;

    PlotModel::Step Step(std::string_view verb, RE::FormID actor, std::string_view actorName)
    {
        PlotModel::Step step;
        step.verb = std::string{verb};
        step.actor = actor;
        step.actorName = std::string{actorName};
        step.slots.push_back({"mastermind", kBoss, "Arcadia"});
        step.slots.push_back({"mark", kHulda, "Hulda"});
        step.slots.push_back({"place", 0x0000A001, "Whiterun"});
        step.slots.push_back({"item", 0x0000D001, "the deed to the Bannered Mare"});
        return step;
    }

    PlotModel::Step Ended(PlotModel::Step step, StepOutcome how)
    {
        step.state = how == StepOutcome::Succeeded ? StepState::Succeeded : StepState::Failed;
        step.outcome = how;
        return step;
    }

    PlotModel::Step Live(PlotModel::Step step)
    {
        step.state = StepState::InProgress;
        return step;
    }

    // Arcadia's plot: the asking around done by `asked` and ended `askedHow`,
    // the rumors under way by `rumors`, and the theft not yet cast.
    PlotState World(RE::FormID asked, RE::FormID rumors, StepOutcome askedHow = StepOutcome::FailedCaught)
    {
        const auto name = [](RE::FormID who) {
            return who == kBoss ? "Arcadia" : who == kYsolda ? "Ysolda" : "Saadia";
        };
        PlotModel::Plot plot;
        plot.id = 1;
        plot.mastermind = kBoss;
        plot.mastermindName = "Arcadia";
        plot.scheme = "Take the Bannered Mare from Hulda.";
        plot.history.push_back(Ended(Step("ask_around", asked, name(asked)), askedHow));
        plot.plan.push_back(Live(Step("spread_rumors", rumors, name(rumors))));
        plot.plan.push_back(Step("steal", 0, ""));

        PlotState state;
        state.plots.push_back(plot);
        state.occupancy[kBoss] = {1, Role::Mastermind};
        if (rumors != kBoss) {
            state.occupancy[rumors] = {1, Role::Actor};
        }
        return state;
    }
} // namespace

TEST_CASE("PlotAwareness::Describe", "[PlotAwareness][engine]")
{
    SECTION("when the mastermind has sent others")
    {
        const auto state = World(kYsolda, kSaadia);

        SECTION("should tell the mastermind the scheme and every step of it, in order")
        {
            const auto told = PlotAwareness::Describe(state, kBoss);
            REQUIRE(told["role"] == "mastermind");
            REQUIRE(told["scheme"] == "take the Bannered Mare from Hulda");
            REQUIRE(told["alone"] == false);
            const auto& steps = told["steps"];
            REQUIRE(steps.size() == 3);
            CHECK(steps[0]["state"] == "done");
            CHECK(steps[0]["outcome"] == "failed_caught");
            CHECK(steps[0]["self"] == false);
            CHECK(steps[0]["agent"] == "Ysolda");
            CHECK(steps[0]["task"]["verb"] == "ask_around");
            CHECK(steps[0]["task"]["fill"]["place"] == "Whiterun");
            CHECK(steps[1]["state"] == "in_progress");
            CHECK(steps[1]["agent"] == "Saadia");
            CHECK(steps[1]["task"]["verb"] == "spread_rumors");
            CHECK(steps[2]["state"] == "todo");
            CHECK(steps[2]["outcome"] == "");
            CHECK(steps[2]["agent"] == "");
            CHECK(steps[2]["task"]["verb"] == "steal");
            CHECK(steps[2]["task"]["fill"]["item"] == "the deed to the Bannered Mare");
        }

        SECTION("should tell the agent only their errand and who sent them")
        {
            const auto told = PlotAwareness::Describe(state, kSaadia);
            REQUIRE(told["role"] == "agent");
            REQUIRE(told["sender"] == "Arcadia");
            REQUIRE(told["task"]["verb"] == "spread_rumors");
            REQUIRE_FALSE(told.contains("scheme"));
        }

        SECTION("should tell an agent whose errand is over nothing")
        {
            // Ysolda's step ended and released her; what she did is in her
            // memories, and she is in no plot now.
            REQUIRE(PlotAwareness::Describe(state, kYsolda).is_null());
        }
    }

    SECTION("when the mastermind has done every step themselves")
    {
        const auto state = World(kBoss, kBoss);

        SECTION("should say they did each, and that the rest is theirs to do too")
        {
            const auto told = PlotAwareness::Describe(state, kBoss);
            REQUIRE(told["alone"] == true);
            REQUIRE(told["steps"][0]["self"] == true);
            REQUIRE(told["steps"][1]["self"] == true);
            REQUIRE(told["steps"][1]["state"] == "in_progress");
        }
    }

    SECTION("when an earlier step ran out of time")
    {
        const auto state = World(kYsolda, kSaadia, StepOutcome::FailedTimeout);

        SECTION("should say that time ran out, not that it went wrong")
        {
            REQUIRE(PlotAwareness::Describe(state, kBoss)["steps"][0]["outcome"] == "failed_timeout");
        }
    }

    SECTION("when an earlier step succeeded")
    {
        const auto state = World(kYsolda, kSaadia, StepOutcome::Succeeded);

        SECTION("should say so, with no regrouping")
        {
            REQUIRE(PlotAwareness::Describe(state, kBoss)["steps"][0]["outcome"] == "succeeded");
        }
    }

    SECTION("when the work is told from either end")
    {
        auto state = World(kYsolda, kSaadia);
        state.plots[0].plan[0] = Live(Step("win_over", kSaadia, "Saadia"));

        SECTION("should give both the same task, for each to be told from their own side")
        {
            REQUIRE(PlotAwareness::Describe(state, kBoss)["steps"][1]["task"]["verb"] == "win_over");
            REQUIRE(PlotAwareness::Describe(state, kSaadia)["task"]["verb"] == "win_over");
        }
    }

    SECTION("when the person is in no plot")
    {
        const auto state = World(kYsolda, kSaadia);

        SECTION("should say nothing")
        {
            REQUIRE(PlotAwareness::Describe(state, kStranger).is_null());
            REQUIRE(PlotAwareness::Describe(state, 0).is_null());
        }
    }

    SECTION("when the plot has ended")
    {
        auto state = World(kYsolda, kSaadia);
        state.plots[0].status = PlotModel::PlotStatus::Failed;

        SECTION("should say nothing to anybody, even before they are released")
        {
            REQUIRE(PlotAwareness::Describe(state, kBoss).is_null());
            REQUIRE(PlotAwareness::Describe(state, kSaadia).is_null());
        }
    }

    SECTION("when an agent is engaged but the live step is somebody else's")
    {
        auto state = World(kYsolda, kSaadia);
        state.occupancy[kStranger] = {1, Role::Actor};

        SECTION("should tell them nothing rather than another person's errand")
        {
            REQUIRE(PlotAwareness::Describe(state, kStranger).is_null());
        }
    }
}
