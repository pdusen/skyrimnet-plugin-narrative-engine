#include <JsonUtils.h>

#include <algorithm>

namespace NarrativeEngine::JsonUtils
{
    int ClampParameterInt(const nlohmann::json& parameters,
                          std::string_view      key,
                          int                   def,
                          int                   lo,
                          int                   hi)
    {
        int value = def;
        if (parameters.is_object()) {
            if (auto it = parameters.find(key);
                it != parameters.end() && it->is_number())
            {
                value = it->get<int>();
            }
        }
        return std::clamp(value, lo, hi);
    }
}
