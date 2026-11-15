#pragma once

#include <LanguageTable.h>

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string_view>

// What a content-producing prompt is told about the language its content is
// to be written in.
//
// The prompts stay English. Each one whose answer has content fields -- text
// the player reads, or that becomes a memory -- includes
// components/narrative_engine/output_language.prompt, which asks for those
// fields in the player's language and for every other field as described.
// It renders nothing under English, so an English prompt is unchanged. See
// "The output-language line" in docs/implementation/PHASE_18_INTERNATIONALIZATION.md.
namespace NarrativeEngine::PromptLanguage
{
    // Set the context's `output_language` (the table's English name for its
    // language), `content_fields` (the answer's content fields, by name), and
    // `prompt_forms` ([{field, form}]: the shape the language's file asks of
    // a field spliced into one of its templates, for each field it gives one).
    void Add(nlohmann::json& ctx,
             std::initializer_list<std::string_view> contentFields,
             const LanguageTable::Table& table);
} // namespace NarrativeEngine::PromptLanguage
