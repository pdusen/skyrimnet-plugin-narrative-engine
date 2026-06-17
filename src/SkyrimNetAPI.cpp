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
        return ::PublicGetRecentEvents(formId, maxCount, eventTypeFilter.c_str());
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
}
