#include <PlotStepParse.h>

#include <cctype>

#include <LLMTextSanitizer.h>

namespace NarrativeEngine::PlotStepParse
{
    namespace
    {
        // One free-text field: present, a string, non-empty after
        // sanitizing, within its ceiling.
        //
        // `optional` relaxes only the non-empty test, leaving the
        // ceiling and the player check exactly where they are. It exists
        // for `target`, which the model may legitimately decline to
        // fill -- see the caller.
        bool ReadText(const nlohmann::json& json,
                      const char* key,
                      std::size_t limit,
                      const std::string& where,
                      std::string& out,
                      std::string& rejection,
                      bool optional = false)
        {
            const auto it = json.find(key);
            if (it == json.end() || !it->is_string()) {
                rejection = where + " carried no '" + std::string{key} + "' string";
                return false;
            }
            // Sanitized AT THE POINT OF EXTRACTION. This text reaches the
            // retrieval index, the co-save and the dashboard, and a
            // smart quote survives all three to become a missing glyph.
            out = LLMTextSanitizer::Sanitize(it->get<std::string>());
            if (out.empty() && !optional) {
                rejection = where + " had a '" + std::string{key} + "' that was empty after sanitizing";
                return false;
            }
            if (out.size() > limit) {
                rejection = where + " had a '" + std::string{key} + "' of " + std::to_string(out.size())
                            + " bytes, over the limit of " + std::to_string(limit);
                return false;
            }
            if (NamesThePlayer(out)) {
                rejection = where + " named the Dragonborn in its '" + std::string{key}
                            + "'; these plots run among the people of Skyrim and cannot involve the player";
                return false;
            }
            return true;
        }
    } // namespace

    bool NamesThePlayer(std::string_view text)
    {
        constexpr std::string_view kWord = "dragonborn";
        for (std::size_t at = 0; at + kWord.size() <= text.size(); ++at) {
            bool same = true;
            for (std::size_t i = 0; i < kWord.size() && same; ++i) {
                same = static_cast<char>(std::tolower(static_cast<unsigned char>(text[at + i]))) == kWord[i];
            }
            if (!same) {
                continue;
            }
            const auto before = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
            const auto after =
                at + kWord.size() >= text.size() || !std::isalpha(static_cast<unsigned char>(text[at + kWord.size()]));
            if (before && after) {
                return true;
            }
        }
        return false;
    }

    bool ReadRoles(const nlohmann::json& raw,
                   const RoleLimits& limits,
                   const std::string& where,
                   PlotModel::Step& step,
                   std::string& rejection)
    {
        // The chart's caption for this step. Required: a plan whose
        // steps all fall back to their type verb reads as a row of
        // repeated words, which is the failure the field exists to fix.
        std::string why;
        if (!ReadText(raw, "label", limits.maxLabel, where, step.label, why)) {
            rejection = why;
            return false;
        }

        // Every step needs somebody to carry it out, so the agent is
        // required.
        const auto agentIt = raw.find("agent");
        if (agentIt == raw.end() || agentIt->is_null()) {
            rejection = where + " needs an 'agent' and carried none";
            return false;
        }
        if (!agentIt->is_object()) {
            rejection = where + ": 'agent' was not an object with a query and a label";
            return false;
        }

        if (!ReadText(*agentIt, "query", limits.maxQuery, where + ", 'agent'", step.agentRole.query, why)) {
            rejection = why;
            return false;
        }
        if (!ReadText(*agentIt, "label", limits.maxLabel, where + ", 'agent'", step.agentRole.label, why)) {
            rejection = why;
            return false;
        }

        // A target is not required: most steps act on a place or a
        // thing, and the prompts are explicit that inventing a person
        // for those is worse than leaving the key out. A plain string,
        // which may be a name or a description of a kind of person --
        // both are only ever text to search the biographies for.
        //
        // THREE WAYS TO SAY NOBODY, all of which mean the same thing:
        // the key absent, the key null, and the key an empty string.
        // Only the first two used to be tolerated, and a model that
        // wrote `"target": ""` lost a whole plan for it -- one plot
        // birth and one adaptation in a single twenty-tick run.
        // PlotModel::Step documents an empty targetWanted as a
        // supported state ("the step acts on no person") and
        // PlotTick::ResolveTarget opens by handling it, so rejecting it
        // here contradicted both.
        const auto targetIt = raw.find("target");
        if (targetIt != raw.end() && !targetIt->is_null()) {
            if (!ReadText(raw, "target", limits.maxQuery, where, step.targetWanted, why, true)) {
                rejection = why;
                return false;
            }
        }
        return true;
    }
} // namespace NarrativeEngine::PlotStepParse
