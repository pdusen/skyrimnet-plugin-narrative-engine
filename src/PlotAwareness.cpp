#include <PlotAwareness.h>

#include <PlotVerbs.h>

#include <string>
#include <string_view>
#include <vector>

namespace NarrativeEngine::PlotAwareness
{
    namespace
    {
        using PlotModel::StepOutcome;
        using PlotModel::StepState;
        using PlotVerbs::Template;

        std::string AgentOf(const PlotModel::Step& step)
        {
            return step.actorName.empty() ? std::string{"someone"} : step.actorName;
        }

        // A step that has ended, as the mastermind heard of it. Every failure
        // in a plot still running was one it went on from, so it says so.
        std::string Ended(const PlotModel::Plot& plot, const PlotModel::Step& step, const std::string& task)
        {
            const bool self = step.actor == plot.mastermind;
            const std::string opening = self ? "You set out to " + task : "You sent " + AgentOf(step) + " to " + task;
            switch (step.outcome) {
            case StepOutcome::Succeeded:
                return opening + (self ? ", and succeeded." : ", and they succeeded.");
            case StepOutcome::FailedTimeout:
                return opening
                       + (self ? ", but ran out of time and had to regroup."
                               : ", but they ran out of time and you had to regroup.");
            case StepOutcome::FailedCaught:
            case StepOutcome::None:
                break;
            }
            return opening + ", but it went wrong and you had to regroup.";
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

        std::string ForMastermind(const PlotModel::Plot& plot)
        {
            const bool alone = WorksAlone(plot);
            std::vector<std::string> lines;
            const auto add = [&](const PlotModel::Step& step) {
                const auto task = PlotVerbs::RenderPhrase(step, Template::MastermindTask);
                if (task.empty()) {
                    return;
                }
                if (step.IsTerminal()) {
                    lines.push_back(Ended(plot, step, task));
                } else if (step.state == StepState::InProgress) {
                    lines.push_back(step.actor == plot.mastermind ? "You are currently trying to " + task + "."
                                                                  : "You have sent " + AgentOf(step) + " to " + task
                                                                        + ". This is currently in progress.");
                } else {
                    lines.push_back(alone ? "You need to " + task + "." : "You need to send someone to " + task + ".");
                }
            };
            for (const auto& step : plot.history) {
                add(step);
            }
            for (std::size_t i = plot.cursor; i < plot.plan.size(); ++i) {
                add(plot.plan[i]);
            }
            if (lines.empty()) {
                return {};
            }

            const auto scheme = PlotVerbs::SchemePhrase(plot.scheme);
            std::string out = scheme.empty() ? "You currently have a plot in motion."
                                             : "You currently have a plot in motion to " + scheme + ".";
            out += " The plot has the following steps:\n";
            for (std::size_t i = 0; i < lines.size(); ++i) {
                out += "\n" + std::to_string(i + 1) + ". " + lines[i];
            }
            return out;
        }

        std::string ForAgent(const PlotModel::Plot& plot, RE::FormID npc)
        {
            const auto* step = plot.LiveStep();
            if (step == nullptr || step->actor != npc) {
                return {};
            }
            const auto task = PlotVerbs::RenderPhrase(*step, Template::AgentTask);
            if (task.empty()) {
                return {};
            }
            const std::string_view sender =
                plot.mastermindName.empty() ? std::string_view{"someone"} : std::string_view{plot.mastermindName};
            return "You have been sent by " + std::string{sender} + " to " + task
                   + ". You're in the process of doing that now.";
        }
    } // namespace

    std::string Describe(const PlotState& state, RE::FormID npc)
    {
        if (npc == 0) {
            return {};
        }
        const auto row = state.occupancy.find(npc);
        if (row == state.occupancy.end() || !row->second.IsEngaged()) {
            return {};
        }
        const auto* plot = state.FindPlot(row->second.plotId);
        if (plot == nullptr || plot->IsTerminal()) {
            return {};
        }
        if (row->second.role == PlotModel::Role::Mastermind) {
            return plot->mastermind == npc ? ForMastermind(*plot) : std::string{};
        }
        return ForAgent(*plot, npc);
    }
} // namespace NarrativeEngine::PlotAwareness
