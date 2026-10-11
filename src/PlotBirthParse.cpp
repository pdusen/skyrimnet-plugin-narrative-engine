#include <PlotBirth.h>

#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <PlotStepParse.h>

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
            if (PlotStepParse::NamesThePlayer(out)) {
                rejection = "the '" + std::string{key}
                            + "' named the Dragonborn; these plots run among the people of Skyrim and cannot "
                              "involve the player";
                return false;
            }
            if (out.size() > limit) {
                rejection = std::string("'") + key + "' was " + std::to_string(out.size())
                            + " characters, over the limit of " + std::to_string(limit);
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
        // The scheme is quoted after "as part of my plan to" in every
        // dispatch memory the plot writes, so it has to be an instruction.
        // "He is secretly funding an excavation" reached the co-save in an
        // audited run; as a memory it would read "my plan to he is ...".
        if (!SchemeOpensWithVerb(out.scheme)) {
            out.rejection = "the scheme must open with a plain verb, as an instruction -- not a name, a pronoun or "
                            "an article: '"
                            + out.scheme + "'";
            out.ambition.clear();
            out.scheme.clear();
            return out;
        }

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

    bool SchemeOpensWithVerb(std::string_view scheme)
    {
        // The first two words, lower-cased, letters only.
        std::vector<std::string> words;
        std::string current;
        for (const char c : scheme) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!current.empty()) {
                    words.push_back(std::move(current));
                    current.clear();
                    if (words.size() == 2) {
                        break;
                    }
                }
            } else if (std::isalpha(static_cast<unsigned char>(c))) {
                current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (!current.empty() && words.size() < 2) {
            words.push_back(std::move(current));
        }
        const auto word = [&words](std::size_t index) { return index < words.size() ? words[index] : std::string{}; };

        const auto first = word(0);
        if (first.empty()) {
            return false;
        }
        // Words a scheme cannot open with if it is an instruction.
        static constexpr std::string_view kNotAVerb[] = {"he",  "she", "they",  "it",   "i",     "we",    "you",
                                                         "his", "her", "their", "its",  "my",    "our",   "the",
                                                         "a",   "an",  "this",  "that", "these", "those", "to"};
        for (const auto bad : kNotAVerb) {
            if (first == bad) {
                return false;
            }
        }
        // A name followed by what they mean to do: "Korir plans to ...".
        static constexpr std::string_view kIntends[] = {
            "is", "was", "will", "has", "had", "plans", "wants", "means", "intends", "would", "could", "must", "seeks"};
        const auto second = word(1);
        for (const auto bad : kIntends) {
            if (second == bad) {
                return false;
            }
        }
        return true;
    }
} // namespace NarrativeEngine::PlotBirth
