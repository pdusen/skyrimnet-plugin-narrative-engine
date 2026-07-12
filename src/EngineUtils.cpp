#include <EngineUtils.h>

#include <RE/Skyrim.h>

namespace NarrativeEngine::EngineUtils
{
    double GetCurrentGameHours()
    {
        auto* calendar = RE::Calendar::GetSingleton();
        return calendar ? static_cast<double>(calendar->GetHoursPassed()) : 0.0;
    }
}
