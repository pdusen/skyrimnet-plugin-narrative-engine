#include <PlotAdapt.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <PlotStepParse.h>

#include <nlohmann/json.hpp>

// The pure half of adaptation. No engine, no logging, no LLM -- so
// every way a revision can be wrong is drivable from a committed
// fixture rather than trusted.
namespace NarrativeEngine::PlotAdapt
{
    namespace
    {
        Outcome Reject(std::string reason)
        {
            Outcome out;
            out.ok = false;
            out.rejection = std::move(reason);
            return out;
        }
    } // namespace

    Outcome Parse(const std::string& response, const Limits& limits)
    {
        const auto body = EvaluationPipeline::StripMarkdownFences(response);
        auto json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            return Reject("response was not a JSON object");
        }

        const auto decisionIt = json.find("decision");
        if (decisionIt == json.end() || !decisionIt->is_string()) {
            return Reject("response carried no 'decision' string");
        }
        const auto decision = decisionIt->get<std::string>();

        // --- Conceding ------------------------------------------------------
        if (decision == "concede") {
            Outcome out;
            out.ok = true;
            out.decision = Decision::Concede;

            const auto reasonIt = json.find("reason");
            if (reasonIt != json.end() && reasonIt->is_string()) {
                out.reason = LLMTextSanitizer::Sanitize(reasonIt->get<std::string>());
                if (out.reason.size() > limits.maxReason) {
                    out.reason.resize(limits.maxReason);
                }
            }
            // A concession with no reason is still a concession. The
            // decision is the load-bearing part; the sentence is what a
            // reader sees, and a missing one is worth a default rather
            // than throwing away a valid answer.
            if (out.reason.empty()) {
                out.reason = "they thought better of it";
            }
            return out;
        }

        if (decision != "revise") {
            return Reject("'decision' was '" + decision + "', which is neither 'revise' nor 'concede'");
        }

        // --- Revising -------------------------------------------------------
        const auto planIt = json.find("plan");
        if (planIt == json.end() || !planIt->is_array()) {
            return Reject("a revision carried no 'plan' array");
        }
        if (planIt->size() < limits.minSteps) {
            return Reject("revised plan had " + std::to_string(planIt->size()) + " step(s), fewer than "
                          + std::to_string(limits.minSteps));
        }
        if (planIt->size() > limits.maxSteps) {
            return Reject("revised plan had " + std::to_string(planIt->size()) + " step(s), more than "
                          + std::to_string(limits.maxSteps));
        }

        std::vector<PlotModel::Step> plan;
        plan.reserve(planIt->size());

        for (std::size_t i = 0; i < planIt->size(); ++i) {
            const auto& raw = (*planIt)[i];
            const auto where = "revised step " + std::to_string(i);
            if (!raw.is_object()) {
                return Reject(where + " was not an object");
            }

            const auto typeIt = raw.find("type");
            if (typeIt == raw.end() || !typeIt->is_string()) {
                return Reject(where + " carried no 'type' string");
            }
            PlotModel::Step step;
            if (!PlotModel::ParseStepType(typeIt->get<std::string>(), step.type)) {
                return Reject(where + " named step type '" + typeIt->get<std::string>()
                              + "', which is not in the manifest");
            }

            // Sanitized at the point of extraction, like every other
            // free-form string we take back from a model.
            const auto descIt = raw.find("description");
            if (descIt == raw.end() || !descIt->is_string()) {
                return Reject(where + " carried no 'description' string");
            }
            step.description = LLMTextSanitizer::Sanitize(descIt->get<std::string>());
            if (step.description.empty()) {
                return Reject(where + " had a 'description' that was empty after sanitizing");
            }
            if (step.description.size() > limits.maxDescription) {
                return Reject(where + " had a 'description' of " + std::to_string(step.description.size())
                              + " characters, over the limit of " + std::to_string(limits.maxDescription));
            }
            // The same roles a step gets at birth. A revision that
            // carried none used to fall through to a random draw --
            // the old behaviour, silently, in the middle of a plot
            // that had been casting deliberately until then.
            std::string why;
            if (!PlotStepParse::ReadRoles(raw, {limits.maxRoleQuery, limits.maxRoleLabel}, where, step, why)) {
                return Reject(why);
            }
            plan.push_back(std::move(step));
        }

        // Adaptation rewrites the path, never the destination -- and now
        // it CANNOT rewrite the destination, because the scheme is a
        // sentence on the Plot rather than a step in the plan. What is
        // left to check is the same rule birth applies: the last step is
        // the one that accomplishes the scheme, and concealment
        // accomplishes nothing.
        if (plan.back().type == PlotModel::StepType::Conceal) {
            return Reject("the revised plan ends in 'conceal'; the last step is the one that accomplishes the "
                          "scheme, and covering your tracks accomplishes nothing");
        }

        Outcome out;
        out.ok = true;
        out.decision = Decision::Revise;
        out.plan = std::move(plan);
        return out;
    }
} // namespace NarrativeEngine::PlotAdapt
