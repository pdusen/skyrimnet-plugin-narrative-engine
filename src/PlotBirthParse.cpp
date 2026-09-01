#include <PlotBirth.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>

#include <nlohmann/json.hpp>

// The pure half of plot birth: response text in, plot-or-rejection out.
// No engine, no logging, no LLM -- so every way a response can be wrong
// is drivable from a committed fixture, which is the only way to be sure
// the wrong ones are actually handled.
namespace NarrativeEngine::PlotBirth
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

        // Read an integer index that the model may have written as a
        // number or as a string. Both are common and neither is wrong
        // enough to throw a plot away over -- "2" and 2 mean the same
        // thing, and rejecting one of them would be rejecting a good
        // plan on a formatting detail.
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
                const auto text = value.get<std::string>();
                if (text.empty()) {
                    return false;
                }
                try {
                    std::size_t consumed = 0;
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

    Outcome Parse(const std::string& response, const PlotMenus::Menus& menus, const Limits& limits)
    {
        // Strip a wrapping markdown fence before parsing. Models wrap
        // their JSON in a code fence often enough that the instruction
        // not to cannot be relied on, and the whole response is
        // discarded when they do.
        const auto body = EvaluationPipeline::StripMarkdownFences(response);

        auto json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            return Reject("response was not a JSON object");
        }

        // --- The ambition ------------------------------------------------
        const auto ambitionIt = json.find("ambition");
        if (ambitionIt == json.end() || !ambitionIt->is_string()) {
            return Reject("response carried no 'ambition' string");
        }
        // Sanitize AT THE POINT OF EXTRACTION, before this text is
        // stored, persisted to the co-save, shown on the dashboard or
        // fed into another prompt. Smart quotes, em-dashes and NBSPs
        // arrive in almost every response and travel a long way.
        auto ambition = LLMTextSanitizer::Sanitize(ambitionIt->get<std::string>());
        if (ambition.empty()) {
            return Reject("'ambition' was empty after sanitizing");
        }
        if (ambition.size() > limits.maxAmbition) {
            return Reject("'ambition' was " + std::to_string(ambition.size()) + " characters, over the limit of "
                          + std::to_string(limits.maxAmbition));
        }

        // --- The plan ------------------------------------------------------
        const auto planIt = json.find("plan");
        if (planIt == json.end() || !planIt->is_array()) {
            return Reject("response carried no 'plan' array");
        }
        if (planIt->size() < limits.minSteps) {
            return Reject("plan had " + std::to_string(planIt->size()) + " step(s), fewer than the minimum of "
                          + std::to_string(limits.minSteps));
        }
        if (planIt->size() > limits.maxSteps) {
            return Reject("plan had " + std::to_string(planIt->size()) + " step(s), more than the maximum of "
                          + std::to_string(limits.maxSteps));
        }

        std::vector<PlotModel::Step> plan;
        plan.reserve(planIt->size());

        for (std::size_t i = 0; i < planIt->size(); ++i) {
            const auto& raw = (*planIt)[i];
            const auto where = "step " + std::to_string(i);
            if (!raw.is_object()) {
                return Reject(where + " was not an object");
            }

            const auto typeIt = raw.find("type");
            if (typeIt == raw.end() || !typeIt->is_string()) {
                return Reject(where + " carried no 'type' string");
            }
            // A membership test against the manifest, not a lookup with
            // a default. An unrecognised type is the model inventing a
            // verb, and a plot built on one has a step nothing can run.
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

            // Which menu a step draws from is decided by its TYPE, which
            // is what lets the model write one `target` field instead of
            // choosing a menu as well. Acquire is about an object;
            // everything else is about a person.
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

        Outcome out;
        out.ok = true;
        out.ambition = std::move(ambition);
        out.plan = std::move(plan);
        return out;
    }
} // namespace NarrativeEngine::PlotBirth
