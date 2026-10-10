#pragma once

#include <nlohmann/json.hpp>

#include <string>
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

    // Read string field `key` from `obj`; return `def` if `obj` is not an
    // object, the key is absent, or the value is not a string.
    //
    // Use this instead of nlohmann's `value(key, def)` on ANY payload that
    // came from an LLM or from SkyrimNet. `value()` only falls back when
    // the key is ABSENT — a key that is present holding `null` throws
    // type_error.302 instead. Both sources emit explicit nulls routinely:
    // SkyrimNet's memory rows carry `"display_name":null` and
    // `"condition_expr":null`, and a model told to include a field only
    // under some condition will just as often write `null` as omit it.
    // A `"duplicate_of":null` on an otherwise perfect gossip verdict threw
    // out of the parse, past the whole tick, and into the dispatcher's
    // catch — losing the tick's simulation step and stranding sixty event
    // claims. Absent and null mean the same thing to every caller here, so
    // they are read the same way.
    std::string StringOr(const nlohmann::json& obj, std::string_view key, std::string def = {});

    // Read numeric field `key` from `obj`; return `def` if `obj` is not an
    // object, the key is absent, or the value is not a number.
    //
    // The numeric counterpart of StringOr, and it exists for the same reason:
    // nlohmann's `value(key, def)` falls back only on an ABSENT key, so a key
    // holding an explicit `null` throws type_error.302 instead. A SkyrimNet
    // event carrying `"gameTime":null` threw straight out of the timeline
    // merge -- losing every event in the batch, not just the malformed one --
    // until this existed. Use it for ANY numeric field on a payload that came
    // from an LLM, from SkyrimNet, or from a third-party plugin.
    //
    // Returns a double because every call site works in game seconds; callers
    // wanting an integer should clamp through ClampParameterInt instead.
    double NumberOr(const nlohmann::json& obj, std::string_view key, double def = 0.0);
} // namespace NarrativeEngine::JsonUtils
