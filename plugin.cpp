#include <Plugin.h>

namespace NarrativeEngine
{
    SKSEPluginLoad(const SKSE::LoadInterface* skse)
    {
        return Startup(skse);
    }
}
