#include <PlotBirth.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>

// The pure half of plot birth: three response texts in, three
// answers-or-rejections out. No engine, no logging, no LLM -- so every
// way a response can be wrong is drivable from a committed fixture,
// which is the only way to be sure the wrong ones are actually handled.
namespace NarrativeEngine::PlotBirth
{
    namespace
    {
        std::string Trim(std::string text)
        {
            const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }

        std::string FormatHex(RE::FormID id)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%X", id);
            return buf;
        }

        // A form id as the other prompts render one: a hex string with
        // an 0x prefix. Two spellings are accepted beyond that, because
        // both turn up and neither is ambiguous once the membership test
        // runs -- a bare hex body ("13BB3") and a plain decimal.
        //
        // FULL CONSUMPTION is the load-bearing part. std::stoul happily
        // parses the "13" out of "13BB3" and stops, so a bare hex id
        // would silently become the decimal 13; requiring the whole
        // string to be consumed is what turns that into a miss rather
        // than a wrong answer.
        bool ReadFormId(const std::string& text, RE::FormID& out)
        {
            const auto attempt = [&text](int base, RE::FormID& value) {
                try {
                    std::size_t consumed = 0;
                    const auto parsed = std::stoul(text, &consumed, base);
                    if (consumed != text.size()) {
                        return false;
                    }
                    value = static_cast<RE::FormID>(parsed);
                    return true;
                } catch (const std::exception&) {
                    return false;
                }
            };
            // base 0 first: it honours an 0x prefix and reads everything
            // else as decimal, which is the pair of spellings that are
            // unambiguous. Bare hex falls through to the explicit 16.
            return attempt(0, out) || attempt(16, out);
        }

        // Every response goes through the same front door: strip a
        // wrapping markdown fence, then insist on an object. Models wrap
        // their JSON often enough that the instruction not to cannot be
        // relied on, and the whole response is discarded when they do.
        bool ReadObject(const std::string& response, nlohmann::json& out)
        {
            out = nlohmann::json::parse(EvaluationPipeline::StripMarkdownFences(response), nullptr, false);
            return !out.is_discarded() && out.is_object();
        }

        // A required free-text field: present, a string, non-empty after
        // sanitizing, and within its ceiling.
        //
        // Sanitized AT THE POINT OF EXTRACTION, before this text is
        // stored, persisted to the co-save, shown on the dashboard or
        // fed into the next call in the chain. Smart quotes, em-dashes
        // and NBSPs arrive in almost every response and travel a long
        // way -- and here they would travel through two more prompts.
        bool ReadText(const nlohmann::json& json,
                      const char* key,
                      std::size_t limit,
                      std::string& out,
                      std::string& rejection)
        {
            const auto it = json.find(key);
            if (it == json.end() || !it->is_string()) {
                rejection = std::string("response carried no '") + key + "' string";
                return false;
            }
            out = LLMTextSanitizer::Sanitize(it->get<std::string>());
            if (out.empty()) {
                rejection = std::string("'") + key + "' was empty after sanitizing";
                return false;
            }
            if (out.size() > limit) {
                rejection = std::string("'") + key + "' was " + std::to_string(out.size())
                            + " characters, over the limit of " + std::to_string(limit);
                return false;
            }
            return true;
        }

        // One role on one step: an object carrying a search `query` and
        // a display `label`, or absent.
        //
        // Absent is legal for an optional role and means the step acts
        // on nobody. Present but half-filled is not: a query with no
        // label cannot degrade when nothing matches, and a label with no
        // query can never match anything in the first place. Both or
        // neither.
        bool ReadRole(const nlohmann::json& raw,
                      const char* key,
                      bool required,
                      const PlanLimits& limits,
                      const std::string& where,
                      PlotModel::Step::Role& out,
                      std::string& rejection)
        {
            const auto it = raw.find(key);
            if (it == raw.end() || it->is_null()) {
                if (required) {
                    rejection = where + " needs an '" + key + "' and carried none";
                    return false;
                }
                return true;
            }
            if (!it->is_object()) {
                rejection = where + ": '" + key + "' was not an object with a query and a label";
                return false;
            }
            std::string why;
            if (!ReadText(*it, "query", limits.maxRoleQuery, out.query, why)) {
                rejection = where + ", '" + key + "': " + why;
                return false;
            }
            if (!ReadText(*it, "label", limits.maxRoleLabel, out.label, why)) {
                rejection = where + ", '" + key + "': " + why;
                return false;
            }
            return true;
        }
    } // namespace

    CastOutcome ParseCast(const std::string& response, const std::vector<RE::FormID>& shortlist)
    {
        CastOutcome out;

        if (shortlist.empty()) {
            out.rejection = "there was nobody on the shortlist to choose from";
            return out;
        }

        nlohmann::json json;
        if (!ReadObject(response, json)) {
            out.rejection = "response was not a JSON object";
            return out;
        }

        const auto it = json.find("choice");
        if (it == json.end()) {
            out.rejection = "response carried no 'choice'";
            return out;
        }

        // The prompt asks for a string, which is how the other prompts
        // render a form id. A model that writes the number bare has
        // still answered the question.
        RE::FormID chosen = 0;
        if (it->is_string()) {
            // Not sanitized: this is an identifier being matched against
            // a closed list, not free text on its way to a save or a
            // screen. Trimmed, because a model that pads with a space
            // has still named the right person.
            const auto named = Trim(it->get<std::string>());
            if (named.empty()) {
                out.rejection = "'choice' was empty";
                return out;
            }
            if (!ReadFormId(named, chosen)) {
                out.rejection = "'choice' was '" + named + "', which is not a form id";
                return out;
            }
        } else if (it->is_number_unsigned()) {
            chosen = static_cast<RE::FormID>(it->get<std::uint64_t>());
        } else {
            out.rejection = "'choice' was neither a form id string nor a number";
            return out;
        }

        // A MEMBERSHIP TEST, not a nearest match. A form id that is not
        // on the list is the model picking somebody who is not there --
        // most likely an NPC it knows and we did not offer -- and there
        // is deliberately no fuzzy resolution here. See
        // docs/prior-art/NAME_RESOLUTION_FAILURE_MODES.md for what that
        // cost the project this one learned from.
        for (std::size_t i = 0; i < shortlist.size(); ++i) {
            if (shortlist[i] == chosen) {
                out.ok = true;
                out.choice = i;
                return out;
            }
        }
        out.rejection = "'choice' named form 0x" + FormatHex(chosen) + ", which is not one of the "
                        + std::to_string(shortlist.size()) + " candidates";
        return out;
    }

    AmbitionOutcome ParseAmbition(const std::string& response, const TextLimits& limits)
    {
        AmbitionOutcome out;

        nlohmann::json json;
        if (!ReadObject(response, json)) {
            out.rejection = "response was not a JSON object";
            return out;
        }

        if (!ReadText(json, "ambition", limits.maxAmbition, out.ambition, out.rejection)) {
            return out;
        }
        if (!ReadText(json, "scheme", limits.maxScheme, out.scheme, out.rejection)) {
            return out;
        }

        // The two must not be the same sentence. When they are, the
        // model has restated the agenda as the scheme -- the failure
        // that produced a whole run of plots whose objective was a
        // paraphrase of their own motive, with nothing pulling the
        // scheme toward being a step TOWARD anything.
        //
        // Compared literally rather than fuzzily on purpose: a real
        // similarity measure would need a threshold nobody has measured,
        // and this catches the case that actually happens.
        if (out.ambition == out.scheme) {
            out.ambition.clear();
            out.scheme.clear();
            out.rejection = "the scheme is word for word the ambition; a scheme is a step toward the agenda, "
                            "not a restatement of it";
            return out;
        }

        out.ok = true;
        return out;
    }

    PlanOutcome ParsePlan(const std::string& response, const PlanLimits& limits)
    {
        PlanOutcome out;

        nlohmann::json json;
        if (!ReadObject(response, json)) {
            out.rejection = "response was not a JSON object";
            return out;
        }

        const auto planIt = json.find("plan");
        if (planIt == json.end() || !planIt->is_array()) {
            out.rejection = "response carried no 'plan' array";
            return out;
        }
        if (planIt->size() < limits.minSteps) {
            out.rejection = "plan had " + std::to_string(planIt->size()) + " step(s), fewer than the minimum of "
                            + std::to_string(limits.minSteps);
            return out;
        }
        if (planIt->size() > limits.maxSteps) {
            out.rejection = "plan had " + std::to_string(planIt->size()) + " step(s), more than the maximum of "
                            + std::to_string(limits.maxSteps);
            return out;
        }

        std::vector<PlotModel::Step> plan;
        plan.reserve(planIt->size());

        for (std::size_t i = 0; i < planIt->size(); ++i) {
            const auto& raw = (*planIt)[i];
            const auto where = "step " + std::to_string(i);
            if (!raw.is_object()) {
                out.rejection = where + " was not an object";
                return out;
            }

            const auto typeIt = raw.find("type");
            if (typeIt == raw.end() || !typeIt->is_string()) {
                out.rejection = where + " carried no 'type' string";
                return out;
            }
            // A membership test against the manifest, not a lookup with
            // a default. An unrecognised type is the model inventing a
            // verb, and a plot built on one has a step nothing can run.
            PlotModel::Step step;
            if (!PlotModel::ParseStepType(typeIt->get<std::string>(), step.type)) {
                out.rejection =
                    where + " named step type '" + typeIt->get<std::string>() + "', which is not in the manifest";
                return out;
            }

            if (!ReadText(raw, "description", limits.maxDescription, step.description, out.rejection)) {
                out.rejection = where + ": " + out.rejection;
                return out;
            }

            // Every step needs somebody to carry it out, so an agent is
            // required. A target is not: most steps act on a place or a
            // thing, and the prompt is explicit that inventing a person
            // for those is worse than leaving the key out.
            if (!ReadRole(raw, "agent", true, limits, where, step.agentRole, out.rejection)) {
                return out;
            }
            // Optional, and a plain string: a name or a description of
            // a kind of person. Absent means the step acts on nobody.
            const auto targetIt = raw.find("target");
            if (targetIt != raw.end() && !targetIt->is_null()) {
                std::string why;
                if (!ReadText(raw, "target", limits.maxRoleQuery, step.targetWanted, why)) {
                    out.rejection = where + ": " + why;
                    return out;
                }
            }
            plan.push_back(std::move(step));
        }

        // The LAST STEP is the one that accomplishes the scheme, and
        // covering your tracks accomplishes nothing. A plan ending there
        // is one that never actually reaches what it was for.
        //
        // Rejected rather than quietly dropped: deleting the step would
        // silently rewrite someone's plan, and a model that ended a
        // scheme on concealment did not understand what the last step is
        // for.
        if (plan.back().type == PlotModel::StepType::Conceal) {
            out.rejection = "the plan ends in 'conceal'; the last step is the one that accomplishes the scheme, "
                            "and covering your tracks accomplishes nothing";
            return out;
        }

        out.ok = true;
        out.plan = std::move(plan);
        return out;
    }
} // namespace NarrativeEngine::PlotBirth
