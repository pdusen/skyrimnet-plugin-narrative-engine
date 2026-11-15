#include <PromptLanguage.h>

#include <string>

namespace NarrativeEngine::PromptLanguage
{
    void Add(nlohmann::json& ctx,
             std::initializer_list<std::string_view> contentFields,
             const LanguageTable::Table& table)
    {
        nlohmann::json fields = nlohmann::json::array();
        nlohmann::json forms = nlohmann::json::array();
        for (const auto field : contentFields) {
            fields.push_back(field);
            if (const auto* form = table.PromptForm(field)) {
                forms.push_back({{"field", field}, {"form", *form}});
            }
        }
        ctx["output_language"] = table.GetHeader().name;
        ctx["content_fields"] = std::move(fields);
        ctx["prompt_forms"] = std::move(forms);
    }
} // namespace NarrativeEngine::PromptLanguage
