#include <PlotAdapt.h>

#include <LLMTextSanitizer.h>
#include <logger.h>
#include <PlotItemPool.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

// The engine-bound half of adaptation: describing the setback to the
// model and making the call. The response handling is in
// PlotAdaptParse.cpp and is engine-free.
namespace NarrativeEngine::PlotAdapt
{
    namespace
    {
        constexpr const char* kPromptName = "narrative_engine_plot_adapt";
        constexpr const char* kVariant = "narrative_engine_composer";

        std::string StandingPhrase(double standing)
        {
            if (standing >= 0.99) {
                return "who leads it";
            }
            if (standing >= 0.6) {
                return "who is senior in it";
            }
            if (standing > 0.0) {
                return "who holds a place in it";
            }
            return {};
        }

        // How far through the step they got, in words rather than a
        // number. "About a third" is something a person can reason
        // about; 0.34 invites the model to do arithmetic on a scale it
        // cannot see the ends of.
        std::string ProgressPhrase(const PlotModel::Step& step)
        {
            if (step.threshold <= 0.0f || step.progress <= 0.0f) {
                return {};
            }
            const double fraction = static_cast<double>(step.progress) / static_cast<double>(step.threshold);
            if (fraction < 0.15) {
                return "barely any";
            }
            if (fraction < 0.4) {
                return "about a third";
            }
            if (fraction < 0.65) {
                return "about half";
            }
            if (fraction < 0.9) {
                return "most";
            }
            return "nearly all";
        }

        // Where the objective sits on the menu it must be named from.
        // The model has to be told the index, because that is the only
        // vocabulary it has -- and if the objective is somehow NOT on
        // the menu, that is worth knowing before the call rather than
        // after a rejection.
        bool ObjectiveIndex(const PlotModel::Step& objective, const PlotMenus::Menus& menus, std::size_t& out)
        {
            if (objective.type == PlotModel::StepType::Acquire) {
                for (std::size_t i = 0; i < menus.items.size(); ++i) {
                    if (menus.items[i].form == objective.target) {
                        out = i;
                        return true;
                    }
                }
                return false;
            }
            for (std::size_t i = 0; i < menus.actors.size(); ++i) {
                if (menus.actors[i].npc == objective.target) {
                    out = i;
                    return true;
                }
            }
            return false;
        }

        Outcome Concede(std::string reason)
        {
            Outcome out;
            out.ok = true;
            out.decision = Decision::Concede;
            out.reason = std::move(reason);
            return out;
        }
    } // namespace

    Outcome Compose(const PlotThread::Token& pt,
                    const PlotModel::Plot& plot,
                    const PlotModel::Step& failed,
                    const PlotMenus::Menus& menus,
                    int attempt,
                    int maxAttempts)
    {
        PlotModel::Step objective;
        objective.type = plot.objectiveType;
        objective.target = plot.objectiveTarget;
        objective.targetName = plot.objectiveTargetName;

        std::size_t objectiveIndex = 0;
        if (!ObjectiveIndex(objective, menus, objectiveIndex)) {
            // The objective is no longer nameable -- its target has left
            // the mastermind's menu, most often because they died or the
            // faction tie that put them there is gone. There is nothing
            // to revise toward.
            return Concede("what they were after had moved out of reach");
        }

        nlohmann::json ctx;

        // Same reason as birth: adaptation is asking what THIS person
        // does next, and that needs the profile as much as the first
        // call did.
        nlohmann::json npc = nlohmann::json::object();
        npc["UUID"] = SkyrimNetAPI::FormIDToUUID(plot.mastermind);
        ctx["npc"] = std::move(npc);

        nlohmann::json boss = nlohmann::json::object();
        boss["name"] = plot.mastermindName;
        ctx["mastermind"] = std::move(boss);
        ctx["ambition"] = plot.ambition;

        nlohmann::json obj = nlohmann::json::object();
        obj["type"] = PlotModel::TypeId(objective.type);
        obj["verb"] = PlotModel::Traits(objective.type).verb;
        obj["target"] = objective.targetName;
        obj["index"] = objectiveIndex;
        ctx["objective"] = std::move(obj);

        // What has already happened, as labels rather than structures.
        // The model is being asked to continue a story, and a story is
        // what it should be reading.
        nlohmann::json history = nlohmann::json::array();
        for (const auto& past : plot.history) {
            history.push_back(PlotModel::Label(past) + " -- " + std::string(PlotModel::StepOutcomeId(past.outcome)));
        }
        ctx["history"] = std::move(history);

        nlohmann::json setback = nlohmann::json::object();
        setback["step"] = PlotModel::Label(failed);
        setback["outcome"] = PlotModel::StepOutcomeId(failed.outcome);
        setback["progress"] = ProgressPhrase(failed);
        setback["caught"] = failed.outcome == PlotModel::StepOutcome::FailedCaught;
        ctx["failure"] = std::move(setback);

        ctx["attempt"] = attempt;
        ctx["max_attempts"] = maxAttempts;

        nlohmann::json actors = nlohmann::json::array();
        for (const auto& actor : menus.actors) {
            nlohmann::json entry = nlohmann::json::object();
            entry["name"] = actor.name;
            entry["relation"] = PlotMenus::RelationPhrase(actor.relation);
            entry["standing"] = StandingPhrase(actor.standing);
            actors.push_back(std::move(entry));
        }
        ctx["actors"] = std::move(actors);

        nlohmann::json items = nlohmann::json::array();
        for (const auto& item : menus.items) {
            nlohmann::json entry = nlohmann::json::object();
            entry["name"] = item.displayName;
            entry["category"] = PlotItemPool::CategoryId(item.category);
            items.push_back(std::move(entry));
        }
        ctx["items"] = std::move(items);

        nlohmann::json stepTypes = nlohmann::json::array();
        for (const auto& traits : PlotModel::kStepTypes) {
            nlohmann::json entry = nlohmann::json::object();
            entry["id"] = traits.id;
            entry["description"] = traits.description;
            stepTypes.push_back(std::move(entry));
        }
        ctx["step_types"] = std::move(stepTypes);

        const Limits limits;
        ctx["min_steps"] = limits.minSteps;
        ctx["max_steps"] = limits.maxSteps;

        const auto result = SkyrimNetAPI::SendCustomPromptToLLM(pt, kPromptName, kVariant, ctx.dump());
        if (!result.ok) {
            return Concede("they ran out of ideas");
        }

        Outcome outcome;
        try {
            outcome = Parse(result.response, menus, objective, limits);
        } catch (const std::exception& e) {
            logger::warn("PlotAdapt: parsing threw for plot {}: {}", plot.id, e.what());
            return Concede("they ran out of ideas");
        }

        if (!outcome.ok) {
            // A rejected revision becomes a concession rather than a
            // stall. The plot has a failed step and no next one; leaving
            // it there would be a scheme that never resolves, which is
            // the worst of the three outcomes for a reader.
            logger::warn("PlotAdapt: rejected a revision for plot {}: {}", plot.id, outcome.rejection);
            return Concede("they ran out of ideas");
        }
        return outcome;
    }
} // namespace NarrativeEngine::PlotAdapt
