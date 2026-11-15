#include <PlotAwareness.h>

#include <PlotVerbs.h>

#include <string>
#include <string_view>

namespace NarrativeEngine::PlotAwareness
{
    namespace
    {
        using PlotModel::StepOutcome;
        using PlotModel::StepState;

        // The work of `step`, as plot_task.prompt reads it: its verb and what
        // fills it. An unfilled slot is left out, and reads as someone,
        // somewhere or something.
        nlohmann::json TaskOf(const PlotModel::Step& step)
        {
            nlohmann::json fill = nlohmann::json::object();
            for (const auto& value : step.slots) {
                if (!value.text.empty() && !fill.contains(value.name)) {
                    fill[value.name] = value.text;
                }
            }
            return {{"verb", step.verb}, {"fill", std::move(fill)}};
        }

        // How a step stands, as the mastermind is told it: done, under way,
        // or still to do.
        std::string_view StateOf(const PlotModel::Step& step)
        {
            if (step.IsTerminal()) {
                return "done";
            }
            return step.state == StepState::InProgress ? "in_progress" : "todo";
        }

        // How a step that has ended went, as plot_awareness.prompt words it.
        // Every failure in a plot still running was one it went on from.
        std::string_view OutcomeOf(const PlotModel::Step& step)
        {
            switch (step.outcome) {
            case StepOutcome::Succeeded:
                return "succeeded";
            case StepOutcome::FailedTimeout:
                return "failed_timeout";
            case StepOutcome::FailedCaught:
            case StepOutcome::None:
                break;
            }
            return "failed_caught";
        }

        // Whether the mastermind has carried out every step so far
        // themselves. They are cast as their own agent only when nobody else
        // can be, so somebody who has had nobody to send yet is told they
        // will have to do the rest themselves too.
        bool WorksAlone(const PlotModel::Plot& plot)
        {
            bool any = false;
            const auto alone = [&](const PlotModel::Step& step) {
                if (step.actor == 0) {
                    return true;
                }
                any = true;
                return step.actor == plot.mastermind;
            };
            for (const auto& step : plot.history) {
                if (!alone(step)) {
                    return false;
                }
            }
            for (std::size_t i = plot.cursor; i < plot.plan.size(); ++i) {
                if (!alone(plot.plan[i])) {
                    return false;
                }
            }
            return any;
        }

        nlohmann::json ForMastermind(const PlotModel::Plot& plot)
        {
            nlohmann::json steps = nlohmann::json::array();
            const auto add = [&](const PlotModel::Step& step) {
                // A step with no verb has no task to tell.
                if (PlotVerbs::Find(step.verb) == nullptr) {
                    return;
                }
                steps.push_back({{"state", StateOf(step)},
                                 {"outcome", step.IsTerminal() ? OutcomeOf(step) : std::string_view{}},
                                 {"self", step.actor == plot.mastermind},
                                 {"agent", step.actorName},
                                 {"task", TaskOf(step)}});
            };
            for (const auto& step : plot.history) {
                add(step);
            }
            for (std::size_t i = plot.cursor; i < plot.plan.size(); ++i) {
                add(plot.plan[i]);
            }
            if (steps.empty()) {
                return nullptr;
            }
            return {{"role", "mastermind"},
                    {"scheme", PlotVerbs::SchemePhrase(plot.scheme)},
                    {"alone", WorksAlone(plot)},
                    {"steps", std::move(steps)}};
        }

        nlohmann::json ForAgent(const PlotModel::Plot& plot, RE::FormID npc)
        {
            const auto* step = plot.LiveStep();
            if (step == nullptr || step->actor != npc || PlotVerbs::Find(step->verb) == nullptr) {
                return nullptr;
            }
            return {{"role", "agent"}, {"sender", plot.mastermindName}, {"task", TaskOf(*step)}};
        }
    } // namespace

    nlohmann::json Describe(const PlotState& state, RE::FormID npc)
    {
        if (npc == 0) {
            return nullptr;
        }
        const auto row = state.occupancy.find(npc);
        if (row == state.occupancy.end() || !row->second.IsEngaged()) {
            return nullptr;
        }
        const auto* plot = state.FindPlot(row->second.plotId);
        if (plot == nullptr || plot->IsTerminal()) {
            return nullptr;
        }
        if (row->second.role == PlotModel::Role::Mastermind) {
            return plot->mastermind == npc ? ForMastermind(*plot) : nullptr;
        }
        return ForAgent(*plot, npc);
    }
} // namespace NarrativeEngine::PlotAwareness
