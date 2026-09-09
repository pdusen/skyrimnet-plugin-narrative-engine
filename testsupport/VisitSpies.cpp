#include "VisitSpies.h"

#include <NPCLetterBeat.h>
#include <NPCVisitBeat.h>

// See VisitSpies.h for why these live beside the harness.

namespace NarrativeEngine::Testing
{
    void VisitSpyState::Reset()
    {
        std::scoped_lock lock(mutex);
        onCooldown.clear();
        memoryWatermarks.clear();
    }

    VisitSpyState& VisitSpies()
    {
        static auto* state = new VisitSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::NPCVisitBeat_Cooldowns
{
    bool IsSenderOnCooldown(RE::FormID sender)
    {
        auto& spies = Testing::VisitSpies();
        std::scoped_lock lock(spies.mutex);
        return spies.onCooldown.contains(sender);
    }

    std::optional<double> GetSenderMemoryWatermarkGameHours(RE::FormID sender)
    {
        auto& spies = Testing::VisitSpies();
        std::scoped_lock lock(spies.mutex);
        const auto it = spies.memoryWatermarks.find(sender);
        return it == spies.memoryWatermarks.end() ? std::nullopt : std::optional<double>{it->second};
    }
} // namespace NarrativeEngine::NPCVisitBeat_Cooldowns

namespace NarrativeEngine::NPCLetterBeat_Cooldowns
{
    bool IsSenderOnCooldown(RE::FormID sender)
    {
        return NPCVisitBeat_Cooldowns::IsSenderOnCooldown(sender);
    }

    std::optional<double> GetSenderMemoryWatermarkGameHours(RE::FormID sender)
    {
        return NPCVisitBeat_Cooldowns::GetSenderMemoryWatermarkGameHours(sender);
    }
} // namespace NarrativeEngine::NPCLetterBeat_Cooldowns
