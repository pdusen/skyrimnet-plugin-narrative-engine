#include <JsonUtils.h>

#include <algorithm>

namespace NarrativeEngine::JsonUtils
{
    int ClampParameterInt(const nlohmann::json& parameters, std::string_view key, int def, int lo, int hi)
    {
        int value = def;
        if (parameters.is_object()) {
            if (auto it = parameters.find(key); it != parameters.end() && it->is_number()) {
                value = it->get<int>();
            }
        }
        return std::clamp(value, lo, hi);
    }

    std::string StringOr(const nlohmann::json& obj, std::string_view key, std::string def)
    {
        if (!obj.is_object()) {
            return def;
        }
        // `is_string()` rather than `!is_null()`: a number or an object
        // under a key we expect to be text is just as unusable as a null,
        // and get<std::string>() would throw on either.
        if (auto it = obj.find(key); it != obj.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return def;
    }

    double NumberOr(const nlohmann::json& obj, std::string_view key, double def)
    {
        if (!obj.is_object()) {
            return def;
        }
        // `is_number()` covers integers and floats alike, and excludes the
        // booleans nlohmann would otherwise happily convert to 0 or 1.
        if (auto it = obj.find(key); it != obj.end() && it->is_number()) {
            return it->get<double>();
        }
        return def;
    }
} // namespace NarrativeEngine::JsonUtils
