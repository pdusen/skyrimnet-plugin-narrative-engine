#include <PlotModel.h>

#include <Localization.h>
#include <PlotText.h>
#include <PlotVerbs.h>

#include <algorithm>

namespace NarrativeEngine::PlotModel
{
    bool ParseStepType(std::string_view id, StepType& out) noexcept
    {
        for (std::size_t i = 0; i < kStepTypeCount; ++i) {
            if (kStepTypes[i].id == id) {
                out = static_cast<StepType>(i);
                return true;
            }
        }
        return false;
    }

    std::string_view StepStateId(StepState s) noexcept
    {
        switch (s) {
        case StepState::Planned:
            return "planned";
        case StepState::AwaitingPlayer:
            return "awaiting_player";
        case StepState::InProgress:
            return "in_progress";
        case StepState::Succeeded:
            return "succeeded";
        case StepState::Failed:
            return "failed";
        }
        return "unknown";
    }

    std::string_view StepOutcomeId(StepOutcome o) noexcept
    {
        switch (o) {
        case StepOutcome::None:
            return "none";
        case StepOutcome::Succeeded:
            return "succeeded";
        case StepOutcome::FailedTimeout:
            return "failed_timeout";
        case StepOutcome::FailedCaught:
            return "failed_caught";
        }
        return "unknown";
    }

    std::string_view PlotStatusId(PlotStatus s) noexcept
    {
        switch (s) {
        case PlotStatus::Active:
            return "active";
        case PlotStatus::Succeeded:
            return "succeeded";
        case PlotStatus::Failed:
            return "failed";
        }
        return "unknown";
    }

    std::string_view PlotOutcomeId(PlotOutcome o) noexcept
    {
        switch (o) {
        case PlotOutcome::None:
            return "none";
        case PlotOutcome::ObjectiveAchieved:
            return "objective_achieved";
        case PlotOutcome::Conceded:
            return "conceded";
        case PlotOutcome::AdaptationCapHit:
            return "adaptation_cap_hit";
        case PlotOutcome::MastermindLost:
            return "mastermind_lost";
        case PlotOutcome::FinishFailed:
            return "finish_failed";
        }
        return "unknown";
    }

    float Step::ProgressFraction() const noexcept
    {
        // A threshold of zero means the step was never sized. Reporting
        // 0 rather than dividing is deliberate: an unsized step has made
        // no progress by definition, and a NaN here would propagate
        // straight into the dashboard's progress ring.
        if (threshold <= 0.0f) {
            return 0.0f;
        }
        return std::clamp(progress / threshold, 0.0f, 1.0f);
    }

    const Step* Plot::LiveStep() const noexcept
    {
        if (status != PlotStatus::Active || cursor >= plan.size()) {
            return nullptr;
        }
        const Step& step = plan[cursor];
        return step.state == StepState::InProgress ? &step : nullptr;
    }

    Step* Plot::LiveStep() noexcept
    {
        return const_cast<Step*>(static_cast<const Plot*>(this)->LiveStep());
    }

    std::string Label(StepType type, std::string_view targetName)
    {
        const std::string_view verb = Traits(type).verb;
        if (targetName.empty()) {
            return std::string(verb);
        }
        std::string out;
        out.reserve(verb.size() + 1 + targetName.size());
        out.append(verb);
        out.push_back(' ');
        out.append(targetName);
        return out;
    }

    std::string Label(const Step& step)
    {
        if (!step.verb.empty()) {
            // In English whatever the player reads: the plot log and the
            // casting ladder's word matching are its readers.
            return PlotVerbs::Find(step.verb) != nullptr ? PlotText::DashboardLine(Localization::English(), step)
                                                         : std::string{};
        }
        if (!step.description.empty()) {
            return step.description;
        }
        return Label(step.type, step.targetName);
    }

    const SlotValue* Step::FindSlot(std::string_view name) const noexcept
    {
        for (const auto& value : slots) {
            if (value.name == name) {
                return &value;
            }
        }
        return nullptr;
    }

    std::string Title(const Plot& plot)
    {
        return plot.scheme;
    }
} // namespace NarrativeEngine::PlotModel
