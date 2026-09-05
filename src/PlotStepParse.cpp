#include <PlotStepParse.h>

#include <LLMTextSanitizer.h>

namespace NarrativeEngine::PlotStepParse
{
    namespace
    {
        // One free-text field: present, a string, non-empty after
        // sanitizing, within its ceiling.
        bool ReadText(const nlohmann::json& json,
                      const char* key,
                      std::size_t limit,
                      const std::string& where,
                      std::string& out,
                      std::string& rejection)
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
            if (out.empty()) {
                rejection = where + " had a '" + std::string{key} + "' that was empty after sanitizing";
                return false;
            }
            if (out.size() > limit) {
                rejection = where + " had a '" + std::string{key} + "' of " + std::to_string(out.size())
                            + " bytes, over the limit of " + std::to_string(limit);
                return false;
            }
            return true;
        }
    } // namespace

    bool ReadRoles(const nlohmann::json& raw,
                   const RoleLimits& limits,
                   const std::string& where,
                   PlotModel::Step& step,
                   std::string& rejection)
    {
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

        std::string why;
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
        const auto targetIt = raw.find("target");
        if (targetIt != raw.end() && !targetIt->is_null()) {
            if (!ReadText(raw, "target", limits.maxQuery, where, step.targetWanted, why)) {
                rejection = why;
                return false;
            }
        }
        return true;
    }
} // namespace NarrativeEngine::PlotStepParse
