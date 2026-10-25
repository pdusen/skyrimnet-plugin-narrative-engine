#include <PlotMemories.h>

#include <PlotCandidates.h>

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
    namespace PlotCandidates = NarrativeEngine::PlotCandidates;
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

    bool IsTarget(const PlotMemories::Memory& m)
    {
        return m.source == "target_failed" || m.source == "target_succeeded" || m.source == "target_noticed";
    }

    // The agent's and the mastermind's memories, or the reached people's.
    std::vector<PlotMemories::Memory> Own(std::vector<PlotMemories::Memory> memories)
    {
        std::erase_if(memories, IsTarget);
        return memories;
    }

    std::vector<PlotMemories::Memory> Reached(std::vector<PlotMemories::Memory> memories)
    {
        std::erase_if(memories, [](const PlotMemories::Memory& m) { return !IsTarget(m); });
        return memories;
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
        SECTION("should give the agent their success and the mastermind the report of it")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto memories =
                    Own(PlotMemories::OnResolve(plot, resolved(verb, kAgent, StepOutcome::Succeeded)));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kAgent, kBoss});
                REQUIRE(Sources(memories)
                        == std::vector<std::string_view>{"agent_succeeded", "mastermind_told_succeeded"});
                REQUIRE(memories[1].type == "KNOWLEDGE");
            }
        }
    }

    SECTION("when a step fails")
    {
        SECTION("should give the agent their failure, and the mastermind nothing until the plot answers it")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto memories =
                    Own(PlotMemories::OnResolve(plot, resolved(verb, kAgent, StepOutcome::FailedCaught)));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kAgent});
                REQUIRE(Sources(memories) == std::vector<std::string_view>{"agent_failed"});
            }
        }
    }

    SECTION("when a step runs out of time")
    {
        SECTION("should give the agent the timeout, and the mastermind nothing until the plot answers it")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                const auto memories =
                    Own(PlotMemories::OnResolve(plot, resolved(verb, kAgent, StepOutcome::FailedTimeout)));
                REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kAgent});
                REQUIRE(Sources(memories) == std::vector<std::string_view>{"agent_timed_out"});
            }
        }
    }

    SECTION("when the mastermind did it themselves")
    {
        SECTION("should give them their success, and no report to themselves")
        {
            const auto memories =
                Own(PlotMemories::OnResolve(plot, resolved(*PlotVerbs::Find("steal"), kBoss, StepOutcome::Succeeded)));
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kBoss});
            REQUIRE(Sources(memories) == std::vector<std::string_view>{"agent_succeeded"});
        }

        SECTION("should hold their failure back until the plot answers it")
        {
            for (const auto how : {StepOutcome::FailedCaught, StepOutcome::FailedTimeout}) {
                REQUIRE(Own(PlotMemories::OnResolve(plot, resolved(*PlotVerbs::Find("steal"), kBoss, how))).empty());
            }
        }
    }

    SECTION("when anybody else is in the step")
    {
        SECTION("should give a memory to exactly those the step reached, by their slot's awareness")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                for (const auto how : {StepOutcome::Succeeded, StepOutcome::FailedCaught, StepOutcome::FailedTimeout}) {
                    INFO(verb.id << " outcome " << static_cast<int>(how));
                    const auto step = resolved(verb, kAgent, how);
                    std::vector<RE::FormID> expected;
                    for (std::size_t i = 0; i < verb.slotCount; ++i) {
                        const auto& slot = verb.slots[i];
                        const bool noticed =
                            how == StepOutcome::Succeeded && !slot.Text(PlotVerbs::TargetTemplate::Noticed).empty();
                        if (slot.kind == PlotVerbs::SlotKind::Person
                            && (noticed || PlotCandidates::AwarenessFired(slot.awareness, how))) {
                            expected.push_back(step.FindSlot(slot.name)->form);
                        }
                    }
                    REQUIRE(Keepers(Reached(PlotMemories::OnResolve(plot, step))) == expected);
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
            const auto failed = resolved(*steal, kAgent, StepOutcome::FailedCaught);
            const auto late = resolved(*steal, kAgent, StepOutcome::FailedTimeout);
            REQUIRE(PlotMemories::OnResolve(plot, failed)[0].importance
                    > PlotMemories::OnResolve(plot, late)[0].importance);
            REQUIRE(PlotMemories::OnFailureAnswered(plot, failed, false)[0].importance
                    > PlotMemories::OnFailureAnswered(plot, late, false)[0].importance);
        }
    }
}

TEST_CASE("PlotMemories when the plot answers a failure", "[PlotMemories][engine]")
{
    const auto plot = Scheme();
    const auto resolved = [](std::string_view verb, RE::FormID agent, StepOutcome how) {
        auto step = Cast(*PlotVerbs::Find(verb), agent);
        step.outcome = how;
        return step;
    };

    SECTION("when it goes on another way")
    {
        const auto memories =
            PlotMemories::OnFailureAnswered(plot, resolved("steal", kAgent, StepOutcome::FailedCaught), true);

        SECTION("should give the mastermind one memory of the failure, ending in going on")
        {
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kBoss});
            REQUIRE(Sources(memories) == std::vector<std::string_view>{"mastermind_told_failed"});
            REQUIRE(
                memories[0].text
                == "Idolaf reported back that Hulda had caught them trying to steal a ruby, but I found another way "
                   "to go on with my plan to ruin Hulda's trade.");
        }

        SECTION("should relate the step's people, as the report alone would")
        {
            REQUIRE(memories[0].related == std::vector<RE::FormID>{kAgent, kMark});
        }
    }

    SECTION("when it does not go on")
    {
        SECTION("should give the mastermind the report alone, for a failure or a timeout")
        {
            const auto failed =
                PlotMemories::OnFailureAnswered(plot, resolved("steal", kAgent, StepOutcome::FailedCaught), false);
            REQUIRE(failed[0].text == "Idolaf reported back that Hulda had caught them trying to steal a ruby.");
            const auto late =
                PlotMemories::OnFailureAnswered(plot, resolved("steal", kAgent, StepOutcome::FailedTimeout), false);
            REQUIRE(Sources(late) == std::vector<std::string_view>{"mastermind_told_timed_out"});
        }
    }

    SECTION("when the mastermind did the step themselves")
    {
        SECTION("should give them their own failure, ending in going on when it does")
        {
            const auto memories =
                PlotMemories::OnFailureAnswered(plot, resolved("steal", kBoss, StepOutcome::FailedCaught), true);
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kBoss});
            REQUIRE(Sources(memories) == std::vector<std::string_view>{"agent_failed"});
            REQUIRE(
                memories[0].text
                == "Hulda caught me trying to steal a ruby from them, but I found another way to go on with my plan "
                   "to ruin Hulda's trade.");
        }
    }

    SECTION("when the step did not fail")
    {
        SECTION("should write nothing")
        {
            REQUIRE(
                PlotMemories::OnFailureAnswered(plot, resolved("steal", kAgent, StepOutcome::Succeeded), true).empty());
        }
    }
}

TEST_CASE("PlotMemories when a plot ends", "[PlotMemories][engine]")
{
    SECTION("when the scheme is brought off, or lost at its finish")
    {
        const auto won = PlotMemories::OnSucceeded(Scheme());
        const auto lost = PlotMemories::OnFinishFailed(Scheme());

        SECTION("should give the mastermind one memory naming only the scheme, relating nobody")
        {
            REQUIRE(Keepers(won) == std::vector<RE::FormID>{kBoss});
            REQUIRE(won[0].text == "I brought off my plan to ruin Hulda's trade.");
            REQUIRE(won[0].related.empty());
            REQUIRE(Keepers(lost) == std::vector<RE::FormID>{kBoss});
            REQUIRE(lost[0].text == "My plan to ruin Hulda's trade came to nothing.");
            REQUIRE(lost[0].related.empty());
        }

        SECTION("should weigh success above loss, and loss above giving up")
        {
            const auto conceded = PlotMemories::OnConceded(Scheme());
            REQUIRE(won[0].importance > lost[0].importance);
            REQUIRE(lost[0].importance > conceded[0].importance);
        }
    }

    SECTION("when the plot has no mastermind")
    {
        SECTION("should write nothing")
        {
            auto orphan = Scheme();
            orphan.mastermind = 0;
            REQUIRE(PlotMemories::OnSucceeded(orphan).empty());
            REQUIRE(PlotMemories::OnFinishFailed(orphan).empty());
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

TEST_CASE("PlotMemories for the people a step reaches", "[PlotMemories][engine]")
{
    const auto plot = Scheme();
    const auto reached = [&plot](std::string_view verb, StepOutcome how) {
        auto step = Cast(*PlotVerbs::Find(verb), kAgent);
        step.outcome = how;
        return Reached(PlotMemories::OnResolve(plot, step));
    };

    SECTION("when an agent is caught at it")
    {
        const auto memories = reached("steal", StepOutcome::FailedCaught);

        SECTION("should give the person caught at a memory of catching them, by name")
        {
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kMark});
            REQUIRE(Sources(memories) == std::vector<std::string_view>{"target_failed"});
            REQUIRE(memories[0].text == "I caught Idolaf trying to steal a ruby from me.");
            REQUIRE(memories[0].related == std::vector<RE::FormID>{kAgent});
        }
    }

    SECTION("when it is done to somebody openly")
    {
        SECTION("should give them a memory of who did it")
        {
            const auto memories = reached("bribe", StepOutcome::Succeeded);
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kMark});
            REQUIRE(Sources(memories) == std::vector<std::string_view>{"target_succeeded"});
            REQUIRE(memories[0].text == "Idolaf paid me to look the other way.");
        }

        SECTION("should relate the mastermind only where the memory names them")
        {
            REQUIRE(reached("win_over", StepOutcome::Succeeded)[0].related == std::vector<RE::FormID>{kAgent, kBoss});
            REQUIRE(reached("bribe", StepOutcome::Succeeded)[0].related == std::vector<RE::FormID>{kAgent});
        }
    }

    SECTION("when a denunciation works")
    {
        const auto memories = reached("denounce", StepOutcome::Succeeded);

        SECTION("should give the accused and the authority each their own memory of it")
        {
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kMark, kOther});
            REQUIRE(memories[0].text == "Idolaf denounced me to Vignar.");
            REQUIRE(memories[1].text == "Idolaf came to me denouncing Hulda, and I believed them.");
            REQUIRE(memories[1].related == std::vector<RE::FormID>{kAgent, kMark});
        }
    }

    SECTION("when a denunciation fails")
    {
        SECTION("should give the authority alone a memory of not being swayed")
        {
            const auto memories = reached("denounce", StepOutcome::FailedCaught);
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kOther});
            REQUIRE(memories[0].text == "Idolaf came to me denouncing Hulda, and I was not convinced.");
        }
    }

    SECTION("when a step runs out of time")
    {
        SECTION("should reach nobody")
        {
            for (const auto& verb : PlotVerbs::kVerbs) {
                INFO(verb.id);
                REQUIRE(reached(verb.id, StepOutcome::FailedTimeout).empty());
            }
        }
    }

    SECTION("when catching somebody is weighed against having it done openly")
    {
        SECTION("should make catching them the more important")
        {
            REQUIRE(reached("steal", StepOutcome::FailedCaught)[0].importance
                    > reached("bribe", StepOutcome::Succeeded)[0].importance);
        }
    }
}

TEST_CASE("PlotMemories for a quiet success that leaves a mark", "[PlotMemories][engine]")
{
    const auto plot = Scheme();
    const auto reached = [&plot](std::string_view verb) {
        auto step = Cast(*PlotVerbs::Find(verb), kAgent);
        step.outcome = StepOutcome::Succeeded;
        return Reached(PlotMemories::OnResolve(plot, step));
    };

    SECTION("when a theft succeeds")
    {
        const auto memories = reached("steal");

        SECTION("should give the person robbed a memory of it, naming and relating nobody")
        {
            REQUIRE(Keepers(memories) == std::vector<RE::FormID>{kMark});
            REQUIRE(Sources(memories) == std::vector<std::string_view>{"target_noticed"});
            REQUIRE(memories[0].text == "Someone stole a ruby from me.");
            REQUIRE(memories[0].related.empty());
        }
    }

    SECTION("when a step that leaves nothing behind succeeds")
    {
        SECTION("should give the person it was done to nothing")
        {
            for (const auto* verb : {"watch", "ask_around", "dig_up", "falsify", "plant"}) {
                INFO(verb);
                REQUIRE(reached(verb).empty());
            }
        }
    }
}

TEST_CASE("PlotMemories combines what one plot gives one person", "[PlotMemories][engine]")
{
    const auto statement = [](RE::FormID keeper,
                              std::string text,
                              float importance,
                              std::string type,
                              std::vector<RE::FormID> related,
                              std::string location,
                              std::string source) {
        PlotMemories::Memory m;
        m.npc = keeper;
        m.text = std::move(text);
        m.importance = importance;
        m.type = std::move(type);
        m.related = std::move(related);
        m.location = std::move(location);
        m.source = std::move(source);
        return m;
    };

    SECTION("when one person is given two statements and another one")
    {
        const auto combined = PlotMemories::Combine({
            statement(kBoss,
                      "I tried to win Indaryn over, but they would not be swayed.",
                      0.75f,
                      "EXPERIENCE",
                      {kMark},
                      "",
                      "agent_failed"),
            statement(kAgent, "I stole a ruby from Hulda.", 0.55f, "EXPERIENCE", {kBoss, kMark}, "", "agent_succeeded"),
            statement(kBoss,
                      "My plan to win over Indaryn came to nothing.",
                      0.65f,
                      "EXPERIENCE",
                      {},
                      "Riften",
                      "mastermind_failed"),
        });

        SECTION("should give each person one memory, in the order they first appear")
        {
            REQUIRE(Keepers(combined) == std::vector<RE::FormID>{kBoss, kAgent});
        }

        SECTION("should join the statements into a paragraph, in the order they were made")
        {
            REQUIRE(combined[0].text
                    == "I tried to win Indaryn over, but they would not be swayed. My plan to win over Indaryn came to "
                       "nothing.");
            REQUIRE(combined[0].source == "agent_failed+mastermind_failed");
        }

        SECTION("should keep the most important statement's weight, everybody related and the first location")
        {
            REQUIRE(combined[0].importance == 0.75f);
            REQUIRE(combined[0].related == std::vector<RE::FormID>{kMark});
            REQUIRE(combined[0].location == "Riften");
        }

        SECTION("should leave somebody with a single statement as they were")
        {
            REQUIRE(combined[1].text == "I stole a ruby from Hulda.");
            REQUIRE(combined[1].related == std::vector<RE::FormID>{kBoss, kMark});
            REQUIRE(combined[1].source == "agent_succeeded");
        }
    }

    SECTION("when the statements differ in kind")
    {
        SECTION("should take the type of the most important")
        {
            const auto combined = PlotMemories::Combine({
                statement(kBoss, "Idolaf reported back.", 0.5f, "KNOWLEDGE", {kAgent}, "", "mastermind_told_succeeded"),
                statement(kBoss, "I brought off my plan.", 0.7f, "EXPERIENCE", {}, "", "mastermind_succeeded"),
            });
            REQUIRE(combined.size() == 1);
            REQUIRE(combined[0].type == "EXPERIENCE");
            REQUIRE(combined[0].importance == 0.7f);
        }
    }
}
