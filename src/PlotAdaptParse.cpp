#include <PlotAdapt.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>

#include <nlohmann/json.hpp>

// The pure half of adaptation. No engine, no logging, no LLM -- so the
// one rule that matters most here, that the objective cannot be
// rewritten, is drivable from a fixture rather than trusted.
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

        bool ReadIndex(const nlohmann::json& value, std::size_t& out)
        {
            if (value.is_number_unsigned()) {
                out = value.get<std::size_t>();
                return true;
            }
            if (value.is_number_integer()) {
                const auto signedValue = value.get<std::int64_t>();
                if (signedValue < 0) {
                    return false;
                }
                out = static_cast<std::size_t>(signedValue);
                return true;
            }
            if (value.is_string()) {
                try {
                    std::size_t consumed = 0;
                    const auto text = value.get<std::string>();
                    const auto parsed = std::stoll(text, &consumed);
                    if (consumed != text.size() || parsed < 0) {
                        return false;
                    }
                    out = static_cast<std::size_t>(parsed);
                    return true;
                } catch (const std::exception&) {
                    return false;
                }
            }
            return false;
        }
    } // namespace

    Outcome Parse(const std::string& response,
                  const PlotMenus::Menus& menus,
                  const PlotModel::Step& objective,
                  const Limits& limits)
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
            PlotModel::StepType type{};
            if (!PlotModel::ParseStepType(typeIt->get<std::string>(), type)) {
                return Reject(where + " named step type '" + typeIt->get<std::string>()
                              + "', which is not in the manifest");
            }

            const auto targetIt = raw.find("target");
            if (targetIt == raw.end()) {
                return Reject(where + " carried no 'target'");
            }
            std::size_t index = 0;
            if (!ReadIndex(*targetIt, index)) {
                return Reject(where + " had a 'target' that is not a whole number");
            }

            PlotModel::Step step;
            step.type = type;
            if (type == PlotModel::StepType::Acquire) {
                if (index >= menus.items.size()) {
                    return Reject(where + " named object " + std::to_string(index) + " of "
                                  + std::to_string(menus.items.size()));
                }
                step.target = menus.items[index].form;
                step.targetName = menus.items[index].displayName;
            } else {
                if (index >= menus.actors.size()) {
                    return Reject(where + " named person " + std::to_string(index) + " of "
                                  + std::to_string(menus.actors.size()));
                }
                step.target = menus.actors[index].npc;
                step.targetName = menus.actors[index].name;
            }
            plan.push_back(std::move(step));
        }

        // THE RULE THIS FILE EXISTS FOR. Adaptation rewrites the path,
        // never the destination.
        //
        // Checked rather than enforced by construction -- the plan is
        // not silently corrected to end on the objective -- because a
        // model that changed the objective did not understand the task,
        // and the rest of what it wrote should be trusted no further
        // than that. Rejecting is the honest answer; quietly fixing it
        // would hide the very thing worth knowing about the prompt.
        const auto& last = plan.back();
        if (last.type != objective.type || last.target != objective.target) {
            return Reject("the revised plan ends on '" + PlotModel::Label(last) + "' instead of the objective '"
                          + PlotModel::Label(objective) + "'; the objective is not rewritable");
        }

        Outcome out;
        out.ok = true;
        out.decision = Decision::Revise;
        out.plan = std::move(plan);
        return out;
    }
} // namespace NarrativeEngine::PlotAdapt
