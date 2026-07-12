#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

// JSON-parsing helpers with the "read one field, clamp to a range,
// fall back to a default on missing / wrong type" shape that shows up
// wherever a beat validates its LLM-supplied parameters.
namespace NarrativeEngine::JsonUtils
{
    // Read integer field `key` from `parameters` (expected to be an
    // object); if present and numeric, clamp to [lo, hi]. If missing,
    // non-numeric, or `parameters` isn't an object, return `def`
    // (also clamped to [lo, hi]).
    int ClampParameterInt(const nlohmann::json& parameters, std::string_view key, int def, int lo, int hi);
} // namespace NarrativeEngine::JsonUtils
