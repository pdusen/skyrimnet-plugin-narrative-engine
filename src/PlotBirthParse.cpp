#include <PlotBirth.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <PlotStepParse.h>
#include <PlotVerbs.h>
#include <TextUtf8.h>

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
    using PlotRejection::Code;
    using PlotRejection::Make;
    using PlotRejection::Rejection;

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
                      Rejection& rejection,
                      const LanguageTable::Table& table,
                      std::string_view playerName = {})
        {
            const auto it = json.find(key);
            if (it == json.end() || !it->is_string()) {
                rejection = Make(Code::MissingText, {{"key", key}});
                return false;
            }
            out = LLMTextSanitizer::Sanitize(it->get<std::string>());
            if (out.empty()) {
                rejection = Make(Code::EmptyText, {{"key", key}});
                return false;
            }
            if (PlotStepParse::NamesThePlayer(out, playerName, table)) {
                rejection = Make(Code::NamesPlayer, {{"key", key}});
                return false;
            }
            if (out.size() > limit) {
                rejection = Make(Code::TooLong, {{"key", key}, {"length", out.size()}, {"limit", limit}});
                return false;
            }
            return true;
        }

        // A line rather than a sentence: no trailing period.
        std::string AsLine(std::string text)
        {
            while (!text.empty() && (text.back() == '.' || std::isspace(static_cast<unsigned char>(text.back())))) {
                text.pop_back();
            }
            return text;
        }

        // Two schemes as the same line, whatever their case, in any script.
        bool SameLine(std::string_view a, std::string_view b)
        {
            return TextUtf8::EqualsFolded(AsLine(std::string{a}), AsLine(std::string{b}));
        }
    } // namespace

    CastOutcome ParseCast(const std::string& response, const std::vector<RE::FormID>& shortlist)
    {
        CastOutcome out;

        if (shortlist.empty()) {
            out.rejection = Make(Code::NoShortlist);
            return out;
        }

        nlohmann::json json;
        if (!ReadObject(response, json)) {
            out.rejection = Make(Code::NotJson);
            return out;
        }

        const auto it = json.find("choice");
        if (it == json.end()) {
            out.rejection = Make(Code::MissingChoice);
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
                out.rejection = Make(Code::EmptyChoice);
                return out;
            }
            if (!ReadFormId(named, chosen)) {
                out.rejection = Make(Code::ChoiceNotFormId, {{"text", named}});
                return out;
            }
        } else if (it->is_number_unsigned()) {
            chosen = static_cast<RE::FormID>(it->get<std::uint64_t>());
        } else {
            out.rejection = Make(Code::ChoiceWrongType);
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
        out.rejection = Make(Code::ChoiceNotListed, {{"form", FormatHex(chosen)}, {"count", shortlist.size()}});
        return out;
    }

    AmbitionOutcome ParseAmbition(const std::string& response,
                                  const TextLimits& limits,
                                  std::string_view playerName,
                                  const RoleResolver& resolveRoles,
                                  const std::vector<std::string>& accomplished,
                                  const LanguageTable::Table& table)
    {
        AmbitionOutcome out;

        nlohmann::json json;
        if (!ReadObject(response, json)) {
            out.rejection = Make(Code::NotJson);
            return out;
        }

        if (!ReadText(json, "ambition", limits.maxAmbition, out.ambition, out.rejection, table, playerName)) {
            return out;
        }
        if (!ReadText(json, "scheme", limits.maxScheme, out.scheme, out.rejection, table, playerName)) {
            out.ambition.clear();
            return out;
        }
        out.scheme = AsLine(PlotVerbs::Plain(out.scheme));

        // Done already, so not a step toward anything: a scheme that
        // succeeded was proposed again, word for word, a dozen ticks later.
        for (const auto& done : accomplished) {
            if (SameLine(out.scheme, done)) {
                out.rejection = Make(Code::SchemeAccomplished, {{"scheme", out.scheme}});
                out.ambition.clear();
                out.scheme.clear();
                return out;
            }
        }

        // The kind of step the scheme ends in, from the list it was shown.
        // Planning puts it last and builds back from it, so a goal the
        // manifest does not have is a plan with nothing to stand on.
        constexpr std::size_t kMaxGoal = 40;
        if (!ReadText(json, "goal", kMaxGoal, out.goal, out.rejection, table)) {
            out.ambition.clear();
            out.scheme.clear();
            return out;
        }
        const auto* goal = PlotVerbs::Find(out.goal);
        if (goal == nullptr) {
            out.rejection = Make(Code::GoalUnknown, {{"goal", out.goal}});
            out.ambition.clear();
            out.scheme.clear();
            out.goal.clear();
            return out;
        }
        // Not listed, and for a reason worth telling the retry: what such a
        // step makes or hides is for a step after it.
        if (!goal->finishes) {
            out.rejection = Make(Code::GoalNotFinishing, {{"goal", out.goal}});
            out.ambition.clear();
            out.scheme.clear();
            out.goal.clear();
            return out;
        }

        // How the scheme ends: the goal written out as an act, with its
        // people and any item. Planning ranks whoever it names first, so it
        // is held to the same instruction form as the scheme.
        if (!ReadText(json, "finish", limits.maxScheme, out.finish, out.rejection, table, playerName)) {
            out.ambition.clear();
            out.scheme.clear();
            out.goal.clear();
            return out;
        }
        // The finish is written from the verb list, so it is where an id
        // turns up: "spread_rumors about the court historian".
        out.finish = AsLine(PlotVerbs::Plain(out.finish));

        if (!SchemeOpensWithVerb(out.finish, table)) {
            out.rejection = Make(Code::FinishNotVerb, {{"finish", out.finish}});
            out.ambition.clear();
            out.scheme.clear();
            out.goal.clear();
            out.finish.clear();
            return out;
        }

        // Who and what fills each role of the goal, named by the slot it
        // fills. The finish says it in prose, and prose does not say which
        // name is which: "expose a merchant's ledger to Jarl Elisif" was
        // planned against Elisif.
        std::vector<PlotModel::SlotValue> named;
        {
            const auto roles = json.find("roles");
            nlohmann::json missing = nlohmann::json::array();
            for (std::size_t i = 0; i < goal->slotCount; ++i) {
                const auto& slot = goal->slots[i];
                std::string text;
                if (roles != json.end() && roles->is_object()) {
                    const auto it = roles->find(std::string{slot.name});
                    if (it != roles->end() && it->is_string()) {
                        text = LLMTextSanitizer::Sanitize(it->get<std::string>());
                    }
                }
                if (text.empty()) {
                    missing.push_back(PlotRejection::Role(goal->id, slot.name));
                    continue;
                }
                if (PlotStepParse::NamesThePlayer(text, playerName, table)) {
                    missing.clear();
                    out.rejection = Make(Code::RoleNamesPlayer, {{"role", PlotRejection::Role(goal->id, slot.name)}});
                    break;
                }
                named.push_back({std::string{slot.name}, 0, std::move(text)});
            }
            if (out.rejection.Empty() && !missing.empty()) {
                out.rejection = Make(Code::RolesMissing, {{"goal", out.goal}, {"missing", std::move(missing)}});
            }
            if (out.rejection.Empty() && resolveRoles) {
                if (const auto problems = resolveRoles(*goal, named, out.roles); !problems.empty()) {
                    nlohmann::json list = nlohmann::json::array();
                    for (const auto& problem : problems) {
                        list.push_back(problem.ToJson());
                    }
                    out.rejection = Make(Code::RolesUnfit, {{"problems", std::move(list)}});
                }
            } else if (out.rejection.Empty()) {
                out.roles = named;
            }
            if (!out.rejection.Empty()) {
                out.ambition.clear();
                out.scheme.clear();
                out.goal.clear();
                out.finish.clear();
                out.roles.clear();
                return out;
            }
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
        // The scheme is quoted after "as part of my plan to" in every
        // dispatch memory the plot writes, so it has to be an instruction.
        // "He is secretly funding an excavation" reached the co-save in an
        // audited run; as a memory it would read "my plan to he is ...".
        if (!SchemeOpensWithVerb(out.scheme, table)) {
            out.rejection = Make(Code::SchemeNotVerb, {{"scheme", out.scheme}});
            out.ambition.clear();
            out.scheme.clear();
            return out;
        }

        if (SameLine(out.ambition, out.scheme)) {
            out.ambition.clear();
            out.scheme.clear();
            out.rejection = Make(Code::SchemeIsAmbition);
            return out;
        }

        out.ok = true;
        return out;
    }

    bool SchemeOpensWithVerb(std::string_view scheme, const LanguageTable::Table& table)
    {
        // Nothing to read as an instruction.
        if (TextUtf8::Words(scheme).empty()) {
            return false;
        }
        const auto& rules = table.Matching();
        const auto* check = rules.Find("Scheme");
        return check == nullptr || check->Passes(scheme, rules.options);
    }
} // namespace NarrativeEngine::PlotBirth
