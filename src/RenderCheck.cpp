#include <RenderCheck.h>

#include <nlohmann/json.hpp>

#include <format>

namespace NarrativeEngine::RenderCheck
{
    Sentinels Make(std::uint64_t seed)
    {
        const auto part = [seed](int shift) { return static_cast<unsigned>((seed >> shift) & 0xFFFFu); };
        return {
            std::format("NE-DEC-{:04X}", part(0)),
            std::format("NE-LOOP-{:04X}", part(16)),
            std::format("NE-SET-{:04X}", part(32)),
        };
    }

    std::string Context(const Sentinels& sentinels)
    {
        return nlohmann::json{
            {"render_check",
             {
                 {"file", "sentinel"},
                 {"loop", nlohmann::json::array({sentinels.loop})},
                 {"set", sentinels.set},
             }},
        }
            .dump();
    }

    std::string DecoratorRecord(const Sentinels& sentinels)
    {
        return nlohmann::json{{"sentinel", sentinels.decorator}}.dump();
    }

    Result Evaluate(std::string_view reply, const Sentinels& sentinels)
    {
        const auto has = [reply](std::string_view sentinel) { return !sentinel.empty() && reply.contains(sentinel); };
        return {
            has(kFileSentinel),
            has(sentinels.decorator),
            has(sentinels.loop) && has(sentinels.set),
        };
    }
} // namespace NarrativeEngine::RenderCheck
