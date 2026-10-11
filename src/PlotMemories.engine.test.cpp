#include <PlotMemories.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// Tests for who remembers what in a plot.
//
// Run across every verb, because the rule is the table's and not any one
// verb's: at each moment exactly the agent and the mastermind keep a memory
// -- or the mastermind alone, when they are their own agent -- each from the
// template that voice is written for. A memory written to the wrong person
// is not an error anybody would ever see; it is somebody in Skyrim quietly
// remembering a thing that did not happen to them.

namespace
{
    namespace PlotMemories = NarrativeEngine::PlotMemories;
    namespace PlotModel = NarrativeEngine::PlotModel;
    namespace PlotVerbs = NarrativeEngine::PlotVerbs;
    using PlotModel::StepOutcome;
    using PlotVerbs::Template;

    constexpr RE::FormID kBoss = 0x00010001;
    constexpr RE::FormID kAgent = 0x00010002;
    constexpr RE::FormID kMark = 0x00010003;
    constexpr RE::FormID kOther = 0x00010004;

    PlotModel::Plot Scheme()
    {
        PlotModel::Plot plot;
        plot.id = 1;
        plot.mastermind = kBoss;
        plot.mastermindName = "Olfrid";
        plot.scheme = "Ruin Hulda's trade.";
        return plot;
    }

    // A step of `verb` with every slot filled and an agent cast.
    PlotModel::Step Cast(const PlotVerbs::Verb& verb, RE::FormID agent)
    {
        PlotModel::Step step;
        step.verb = std::string{verb.id};
        step.type = verb.profile;
        step.actor = agent;
        step.actorName = agent == kBoss ? "Olfrid" : "Idolaf";
        step.slots.push_back({"mastermind", kBoss, "Olfrid"});
        bool firstPerson = true;
        for (std::size_t i = 0; i < verb.slotCount; ++i) {
            const auto& slot = verb.slots[i];
            switch (slot.kind) {
            case PlotVerbs::SlotKind::Person:
                step.slots.push_back(
                    {std::string{slot.name}, firstPerson ? kMark : kOther, firstPerson ? "Hulda" : "Vignar"});
                firstPerson = false;
                break;
            case PlotVerbs::SlotKind::Place:
                step.slots.push_back({std::string{slot.name}, 0x0000A001, "Whiterun"});
                break;
            case PlotVerbs::SlotKind::Thing:
                step.slots.push_back({std::string{slot.name}, 0x0000D001, "a ruby"});
                break;
            }
        }
        return step;
    }

    std::vector<RE::FormID> Keepers(const std::vector<PlotMemories::Memory>& memories)
    {
        std::vector<RE::FormID> out;
        for (const auto& m : memories) {
            out.push_back(m.npc);
        }
        return out;
    }

    std::vector<std::string_view> Sources(const std::vector<PlotMemories::Memory>& memories)
    {
        std::vector<std::string_view> out;
        for (const auto& m : memories) {
            out.push_back(m.source);
        }
        return out;
    }
} // namespace

TEST_CASE("PlotMemories at dispatch", "[PlotMemories][engine]")
{
    const auto plot = Scheme();

    SECTION("when somebody is sent")
    {
        SECTION("should give the agent and the mastermind one memory each, in their own voices")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto step = Cast(verb, kAgent);
                const auto memories = PlotMemories::OnDispatch(plot, step);
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kAgent, kBoss});
                REQUIRE(Sources(memories)
                        == std::vector<std::string_view>{"agent_dispatched", "mastermind_dispatched"});
                REQUIRE(memories[0].text == PlotVerbs::RenderStep(step, Template::AgentDispatched, plot.scheme));
                REQUIRE(memories[1].text == PlotVerbs::RenderStep(step, Template::MastermindDispatched, plot.scheme));
            }
        }
    }

    SECTION("when the mastermind does it themselves")
    {
        SECTION("should give them a single memory of deciding to")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto memories = PlotMemories::OnDispatch(plot, Cast(verb, kBoss));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kBoss});
                REQUIRE(Sources(memories) == std::vector<std::string_view>{"self_dispatched"});
            }
        }
    }

    SECTION("when a memory is written")
    {
        const auto* watch = PlotVerbs::Find("watch");
        const auto memories = PlotMemories::OnDispatch(plot, Cast(*watch, kAgent));

        SECTION("should read as a finished sentence, with the scheme as its motive")
        {
            REQUIRE(memories[1].text
                    == "I sent Idolaf to keep watch on Hulda and learn their habits, as part of my plan to ruin "
                       "Hulda's trade.");
        }

        SECTION("should relate everybody else in the step, never the keeper")
        {
            REQUIRE(memories[0].related == std::vector<RE::FormID>{kBoss, kMark});
            REQUIRE(memories[1].related == std::vector<RE::FormID>{kAgent, kMark});
        }

        SECTION("should name no location for a verb without a place")
        {
            REQUIRE(memories[0].location.empty());
        }
    }

    SECTION("when the verb has a place")
    {
        const auto memories = PlotMemories::OnDispatch(plot, Cast(*PlotVerbs::Find("ask_around"), kAgent));

        SECTION("should name it as the location")
        {
            REQUIRE(memories[0].location == "Whiterun");
        }
    }
}

TEST_CASE("PlotMemories at resolution", "[PlotMemories][engine]")
{
    const auto plot = Scheme();

    const auto resolved = [](const PlotVerbs::Verb& verb, RE::FormID agent, StepOutcome how) {
        auto step = Cast(verb, agent);
        step.outcome = how;
        return step;
    };

    SECTION("when a step succeeds")
    {
        SECTION("should give the agent alone a memory of succeeding")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto memories = PlotMemories::OnResolve(plot, resolved(verb, kAgent, StepOutcome::Succeeded));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kAgent});
                REQUIRE(Sources(memories) == std::vector<std::string_view>{"agent_succeeded"});
            }
        }
    }

    SECTION("when a step fails")
    {
        SECTION("should give the agent their failure and the mastermind the report of it")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto memories = PlotMemories::OnResolve(plot, resolved(verb, kAgent, StepOutcome::FailedCaught));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kAgent, kBoss});
                REQUIRE(Sources(memories) == std::vector<std::string_view>{"agent_failed", "mastermind_told_failed"});
            }
        }
    }

    SECTION("when a step runs out of time")
    {
        SECTION("should give the agent the timeout and the mastermind the report of it")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto memories = PlotMemories::OnResolve(plot, resolved(verb, kAgent, StepOutcome::FailedTimeout));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kAgent, kBoss});
                REQUIRE(Sources(memories)
                        == std::vector<std::string_view>{"agent_timed_out", "mastermind_told_timed_out"});
            }
        }
    }

    SECTION("when the mastermind did it themselves")
    {
        SECTION("should give them one memory of how it went, and no report to themselves")
        {
            for (const auto how : {StepOutcome::Succeeded, StepOutcome::FailedCaught, StepOutcome::FailedTimeout}) {
                const auto memories = PlotMemories::OnResolve(plot, resolved(*PlotVerbs::Find("steal"), kBoss, how));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kBoss});
            }
        }
    }

    SECTION("when anybody else is in the step")
    {
        SECTION("should give them nothing")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (const auto how : {StepOutcome::Succeeded, StepOutcome::FailedCaught, StepOutcome::FailedTimeout}) {
                    for (const auto keeper : Keepers(PlotMemories::OnResolve(plot, resolved(verb, kAgent, how)))) {
                        REQUIRE(keeper != kMark);
                        REQUIRE(keeper != kOther);
                    }
                }
            }
        }
    }

    SECTION("when a step has not ended")
    {
        SECTION("should write nothing")
        {
            REQUIRE(
                PlotMemories::OnResolve(plot, resolved(*PlotVerbs::Find("watch"), kAgent, StepOutcome::None)).empty());
        }
    }

    SECTION("when failing is weighed against running out of time")
    {
        SECTION("should make the failure the more important of the two, for both voices")
        {
            const auto* steal = PlotVerbs::Find("steal");
            const auto failed = PlotMemories::OnResolve(plot, resolved(*steal, kAgent, StepOutcome::FailedCaught));
            const auto late = PlotMemories::OnResolve(plot, resolved(*steal, kAgent, StepOutcome::FailedTimeout));
            REQUIRE(failed[0].importance > late[0].importance);
            REQUIRE(failed[1].importance > late[1].importance);
        }
    }
}

TEST_CASE("PlotMemories when a plot is given up", "[PlotMemories][engine]")
{
    SECTION("when conceded")
    {
        const auto memories = PlotMemories::OnConceded(Scheme());

        SECTION("should give the mastermind one memory of giving the scheme up")
        {
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kBoss});
            REQUIRE(memories[0].text == "I gave up on my plan to ruin Hulda's trade.");
        }
    }
}
