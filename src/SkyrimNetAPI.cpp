#include <SkyrimNetAPI.h>

// PublicAPI.h defines its SkyrimNet function pointers at file scope. This is
// the ONLY translation unit in NarrativeEngine that includes it — including
// it from a second .cpp would cause multiple-definition link errors.
#include <PublicAPI.h>

#include <AsyncDispatch.h>
#include <logger.h>

#include <future>
#include <memory>
#include <utility>

namespace NarrativeEngine::SkyrimNetAPI
{
    namespace
    {
        bool g_initialized = false;
        bool g_available = false;
    } // namespace

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
            logger::error("SkyrimNetAPI: SkyrimNet.dll not found (or PublicGetVersion missing). "
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
        std::function<void(const PluginThread::Token&, std::string response, bool success)> callback)
    {
        if (!::PublicSendCustomPromptToLLM) {
            return false;
        }

        // SkyrimNet's callback signature is `void(const char* response,
        // int success)` where `response` is only valid for the duration
        // of the call. This adapter runs on SkyrimNet's foreign worker
        // thread — it has no PluginThread::Token, so the only thing it
        // is compile-legal to do with the caller's callback is enqueue
        // its invocation onto the plugin thread via
        // AsyncDispatch::EnqueueWork. There is NO syntactically-valid
        // path that invokes `cb` on the foreign thread; the type system
        // enforces the discipline.
        auto adapted = [cb = std::move(callback)](const char* response, int success) {
            if (!cb) {
                return;
            }
            AsyncDispatch::EnqueueWork(
                [cb, response = response ? std::string{response} : std::string{}, success = success != 0](
                    const PluginThread::Token& pt) { cb(pt, response, success); });
        };

        return ::PublicSendCustomPromptToLLM(
            promptName.c_str(), variant.c_str(), contextJson.c_str(), std::move(adapted));
    }

    LLMResult SendCustomPromptToLLM(const PluginThread::Token&,
                                    const std::string& promptName,
                                    const std::string& variant,
                                    const std::string& contextJson)
    {
        // shared_ptr rather than a stack-local promise so the
        // lambda's copy of the reference keeps the promise alive
        // even in the pathological case where SkyrimNet fires the
        // callback after we've returned (e.g. queue-full path
        // followed by a delayed delivery). Cheap; the shared_ptr
        // dies as soon as the callback and the caller both drop
        // their references.
        auto promise = std::make_shared<std::promise<LLMResult>>();
        auto future = promise->get_future();

        const bool queued = SendCustomPromptToLLMForeign(
            promptName, variant, contextJson, [promise](std::string response, bool success) {
                LLMResult result;
                result.ok = success;
                result.response = std::move(response);
                try {
                    promise->set_value(std::move(result));
                } catch (const std::future_error&) {
                    // Already set — SkyrimNet fired the callback
                    // twice (should not happen, but be defensive).
                    // Silently drop the second delivery.
                }
            });

        if (!queued) {
            // SendCustomPromptToLLMForeign returned false — either
            // SkyrimNet is unavailable or the queue rejected the
            // task. Return a synthetic failure result. Do NOT
            // block on the future — the callback will never fire.
            return LLMResult{false, "SendCustomPromptToLLM queue full or SkyrimNet unavailable"};
        }

        // Blocks the plugin thread until the foreign-thread callback
        // above pokes the promise.
        return future.get();
    }

    bool SendCustomPromptToLLMForeign(const std::string& promptName,
                                      const std::string& variant,
                                      const std::string& contextJson,
                                      std::function<void(std::string response, bool success)> foreignCallback)
    {
        if (!::PublicSendCustomPromptToLLM) {
            return false;
        }

        // Deliberately NO AsyncDispatch::EnqueueWork bridge here —
        // this variant exists specifically so the callback can fire
        // on the foreign thread. See the header for why (used
        // internally by the sync SendCustomPromptToLLM overload
        // above; routing the promise-set through AsyncDispatch would
        // deadlock).
        //
        // Response C-string is copied into std::string before being
        // handed to the caller's callback; the original is only valid
        // for the duration of the SkyrimNet-side call.
        auto adapted = [cb = std::move(foreignCallback)](const char* response, int success) {
            if (cb) {
                cb(response ? std::string{response} : std::string{}, success != 0);
            }
        };

        return ::PublicSendCustomPromptToLLM(
            promptName.c_str(), variant.c_str(), contextJson.c_str(), std::move(adapted));
    }

    std::string GetRecentEvents(std::uint32_t formId, int maxCount, const std::string& eventTypeFilter)
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

    bool RegisterDecorator(const std::string& name,
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
        if (!::PublicIsMemorySystemReady)
            return false;
        try {
            return ::PublicIsMemorySystemReady();
        } catch (...) {
            return false;
        }
    }

    std::uint64_t FormIDToUUID(std::uint32_t formId)
    {
        if (!::PublicFormIDToUUID)
            return 0;
        try {
            return ::PublicFormIDToUUID(formId);
        } catch (...) {
            logger::warn("SkyrimNetAPI::FormIDToUUID: exception across DLL boundary");
            return 0;
        }
    }

    std::string GetActorEngagement(int maxCount,
                                   bool excludePlayer,
                                   bool playerEventsOnly,
                                   double shortWindowSeconds,
                                   double mediumWindowSeconds)
    {
        if (!::PublicGetActorEngagement)
            return "[]";
        if (!::PublicIsMemorySystemReady || !::PublicIsMemorySystemReady()) {
            return "[]";
        }
        try {
            return ::PublicGetActorEngagement(
                maxCount, excludePlayer, playerEventsOnly, shortWindowSeconds, mediumWindowSeconds);
        } catch (...) {
            logger::warn("SkyrimNetAPI::GetActorEngagement: exception across DLL boundary");
            return "[]";
        }
    }

    std::string GetMemoriesForActor(std::uint32_t formId, int maxCount, const std::string& contextQuery)
    {
        if (!::PublicGetMemoriesForActor)
            return "[]";
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

    std::string GetRecentDialogue(std::uint32_t formId, int maxExchanges)
    {
        if (!::PublicGetRecentDialogue)
            return "[]";
        if (!::PublicIsMemorySystemReady || !::PublicIsMemorySystemReady()) {
            return "[]";
        }
        try {
            return ::PublicGetRecentDialogue(formId, maxExchanges);
        } catch (...) {
            logger::warn("SkyrimNetAPI::GetRecentDialogue: exception across DLL boundary");
            return "[]";
        }
    }

    int AddMemory(std::uint32_t formId,
                  const std::string& contentText,
                  float importance,
                  const std::string& memoryType,
                  const std::string& emotion,
                  const std::string& location,
                  const std::string& tagsJson,
                  const std::string& relatedActorsJson)
    {
        if (!::PublicAddMemory)
            return -1;
        if (!::PublicIsMemorySystemReady || !::PublicIsMemorySystemReady()) {
            return -1;
        }
        // SkyrimNet documents nullptr as "no tags / no related actors".
        // An empty C-string routes through its JSON-parse branch and
        // is either rejected outright or (worse) treated as parse
        // failure that silently drops the write. Mirror the
        // nullptr-on-empty convention used by every other query in
        // this file.
        const char* tagsPtr = tagsJson.empty() ? nullptr : tagsJson.c_str();
        const char* relatedPtr = relatedActorsJson.empty() ? nullptr : relatedActorsJson.c_str();
        try {
            return ::PublicAddMemory(formId,
                                     contentText.c_str(),
                                     importance,
                                     memoryType.c_str(),
                                     emotion.c_str(),
                                     location.c_str(),
                                     tagsPtr,
                                     relatedPtr);
        } catch (...) {
            logger::warn("SkyrimNetAPI::AddMemory: exception across DLL boundary");
            return -1;
        }
    }
} // namespace NarrativeEngine::SkyrimNetAPI
