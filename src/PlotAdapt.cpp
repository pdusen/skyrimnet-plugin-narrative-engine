#include <PlotAdapt.h>

#include <GossipGraph.h>
#include <logger.h>
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
        // The director, like the three birth calls. Revise-or-concede is
        // a bounded decision with a structured answer, not a passage of
        // prose.
        constexpr const char* kVariant = "narrative_engine_director";

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
                    int attempt,
                    int maxAttempts)
    {
        nlohmann::json ctx;

        // Same reason as birth: adaptation is asking what THIS person
        // does next, and that needs the profile as much as the first
        // call did -- and via the ACTOR REFERENCE, for the same reason.
        // `plot.mastermind` is a base form; SkyrimNet does not know
        // those, and hands back 0 for every one of them.
        const auto actorRef = GossipGraph::ActorRefFor(plot.mastermind);
        const auto uuid = actorRef != 0 ? SkyrimNetAPI::FormIDToUUID(actorRef) : 0;
        if (uuid == 0) {
            logger::warn("PlotAdapt: no SkyrimNet UUID for {} (base 0x{:X}, ref 0x{:X}); the revision will be "
                         "judged without a character profile",
                         plot.mastermindName,
                         plot.mastermind,
                         actorRef);
        }
        nlohmann::json npc = nlohmann::json::object();
        npc["UUID"] = uuid;
        ctx["npc"] = std::move(npc);

        nlohmann::json boss = nlohmann::json::object();
        boss["name"] = plot.mastermindName;
        ctx["mastermind"] = std::move(boss);

        ctx["ambition"] = plot.ambition;
        // The destination, as the sentence birth wrote. Not a step, and
        // not something a revision can reach -- it is here to be aimed
        // at, not chosen from.
        ctx["scheme"] = plot.scheme;

        // THE PLAN BEING REVISED. Absent until now, which is why nothing
        // ever changed: the model was shown what had already happened
        // and what the scheme was, and then asked for a revision without
        // ever being told what it was revising. With no plan in front of
        // it, it re-derived one from the same inputs that produced the
        // original, and unsurprisingly wrote the same steps back.
        //
        // The failed step is NOT here -- it has already moved to
        // history. This is only what they were still intending to do.
        nlohmann::json remaining = nlohmann::json::array();
        for (std::size_t i = plot.cursor; i < plot.plan.size(); ++i) {
            remaining.push_back(PlotModel::Label(plot.plan[i]));
        }
        ctx["remaining"] = std::move(remaining);

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

        // The manifest, verbatim, as birth does it.
        nlohmann::json stepTypes = nlohmann::json::array();
        for (const auto& traits : PlotModel::kStepTypes) {
            nlohmann::json entry = nlohmann::json::object();
            entry["id"] = traits.id;
            entry["description"] = traits.description;
            stepTypes.push_back(std::move(entry));
        }
        ctx["step_types"] = std::move(stepTypes);

        const Limits limits;

        const auto result = SkyrimNetAPI::SendCustomPromptToLLM(pt, kPromptName, kVariant, ctx.dump());
        if (!result.ok) {
            return Concede("they ran out of ideas");
        }

        Outcome outcome;
        try {
            outcome = Parse(result.response, limits);
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
