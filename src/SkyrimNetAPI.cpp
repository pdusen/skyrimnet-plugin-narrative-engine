#include <SkyrimNetAPI.h>

// PublicAPI.h defines its SkyrimNet function pointers at file scope. This is
// the ONLY translation unit in NarrativeEngine that includes it — including
// it from a second .cpp would cause multiple-definition link errors.
#include <PublicAPI.h>

#include <logger.h>

#include <utility>

namespace NarrativeEngine::SkyrimNetAPI
{
    namespace
    {
        bool g_initialized = false;
        bool g_available = false;
    }

    bool Initialize()
    {
        if (g_initialized) {
            return g_available;
        }
        g_initialized = true;

        g_available = ::FindFunctions();
        if (g_available) {
            const int version = ::PublicGetVersion ? ::PublicGetVersion() : -1;
            logger::info("SkyrimNetAPI: initialized (version={})", version);
        } else {
            logger::error(
                "SkyrimNetAPI: SkyrimNet.dll not found (or PublicGetVersion missing). "
                "Director loop will run but evaluations will be skipped.");
        }
        return g_available;
    }

    bool IsAvailable()
    {
        return g_available;
    }

    int GetVersion()
    {
        if (!g_available || !::PublicGetVersion) {
            return -1;
        }
        return ::PublicGetVersion();
    }

    bool SendCustomPromptToLLM(
        const std::string& promptName,
        const std::string& variant,
        const std::string& contextJson,
        std::function<void(std::string response, bool success)> callback)
    {
        if (!::PublicSendCustomPromptToLLM) {
            return false;
        }

        // SkyrimNet's callback signature is `void(const char* response, int success)`
        // where `response` is only valid for the duration of the call. Copy it
        // into a std::string so our caller can safely outlive that scope.
        auto adapted = [cb = std::move(callback)](const char* response, int success) {
            if (cb) {
                cb(response ? std::string{response} : std::string{}, success != 0);
            }
        };

        return ::PublicSendCustomPromptToLLM(
            promptName.c_str(),
            variant.c_str(),
            contextJson.c_str(),
            std::move(adapted));
    }

    std::string GetRecentEvents(
        std::uint32_t formId,
        int maxCount,
        const std::string& eventTypeFilter)
    {
        if (!::PublicGetRecentEvents) {
            return "[]";
        }
        // SkyrimNet's PublicAPI.h example explicitly gates event/memory
        // queries behind PublicIsMemorySystemReady() — calling them before
        // the database finishes initializing crashes. The DB isn't ready
        // immediately at kPostLoadGame for saves with prior history; it
        // rebuilds asynchronously over some seconds. Returning "[]" here
        // until it's ready matches the doc's "return empty results gracefully
        // if called before the database is ready" contract.
        if (!::PublicIsMemorySystemReady || !::PublicIsMemorySystemReady()) {
            return "[]";
        }
        // SkyrimNet expects nullptr (not "") for "no filter" — passing an
        // empty C-string makes it route through the filtering branch and
        // crash. IntelEngine's MemoryDB::GetFormattedRecentEvents uses the
        // same nullptr-on-empty pattern. The try/catch guards against any
        // ABI-edge throw across the DLL boundary.
        const char* filter = eventTypeFilter.empty() ? nullptr : eventTypeFilter.c_str();
        try {
            return ::PublicGetRecentEvents(formId, maxCount, filter);
        } catch (...) {
            logger::warn("SkyrimNetAPI::GetRecentEvents: exception across DLL boundary");
            return "[]";
        }
    }

    bool RegisterDecorator(
        const std::string& name,
        const std::string& description,
        std::function<std::string(RE::Actor*)> callback)
    {
        if (!::PublicRegisterDecorator) {
            return false;
        }
        return ::PublicRegisterDecorator(name.c_str(), description.c_str(), std::move(callback));
    }

    bool HasDecorator(const std::string& name)
    {
        if (!::PublicHasDecorator) {
            return false;
        }
        return ::PublicHasDecorator(name.c_str());
    }

    bool IsMemorySystemReady()
    {
        if (!::PublicIsMemorySystemReady) return false;
        try {
            return ::PublicIsMemorySystemReady();
        } catch (...) {
            return false;
        }
    }

    std::string GetActorEngagement(int    maxCount,
                                   bool   excludePlayer,
                                   bool   playerEventsOnly,
                                   double shortWindowSeconds,
                                   double mediumWindowSeconds)
    {
        if (!::PublicGetActorEngagement) return "[]";
        if (!::PublicIsMemorySystemReady || !::PublicIsMemorySystemReady()) {
            return "[]";
        }
        try {
            return ::PublicGetActorEngagement(
                maxCount, excludePlayer, playerEventsOnly,
                shortWindowSeconds, mediumWindowSeconds);
        } catch (...) {
            logger::warn("SkyrimNetAPI::GetActorEngagement: exception across DLL boundary");
            return "[]";
        }
    }

    std::string GetMemoriesForActor(std::uint32_t      formId,
                                    int                maxCount,
                                    const std::string& contextQuery)
    {
        if (!::PublicGetMemoriesForActor) return "[]";
        if (!::PublicIsMemorySystemReady || !::PublicIsMemorySystemReady()) {
            return "[]";
        }
        // SkyrimNet's contextQuery accepts nullptr for "no semantic
        // bias, recency-only ranking." Mirroring the GetRecentEvents
        // pattern, pass nullptr for empty string.
        const char* q = contextQuery.empty() ? nullptr : contextQuery.c_str();
        try {
            return ::PublicGetMemoriesForActor(formId, maxCount, q);
        } catch (...) {
            logger::warn("SkyrimNetAPI::GetMemoriesForActor: exception across DLL boundary");
            return "[]";
        }
    }

    int AddMemory(std::uint32_t      formId,
                  const std::string& contentText,
                  float              importance,
                  const std::string& memoryType,
                  const std::string& emotion,
                  const std::string& location)
    {
        if (!::PublicAddMemory) return -1;
        if (!::PublicIsMemorySystemReady || !::PublicIsMemorySystemReady()) {
            return -1;
        }
        try {
            return ::PublicAddMemory(
                formId,
                contentText.c_str(),
                importance,
                memoryType.c_str(),
                emotion.c_str(),
                location.c_str(),
                /*tagsJSON=*/nullptr,
                /*relatedActorsJSON=*/nullptr);
        } catch (...) {
            logger::warn("SkyrimNetAPI::AddMemory: exception across DLL boundary");
            return -1;
        }
    }
}
