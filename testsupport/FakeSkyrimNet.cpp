#include "FakeSkyrimNet.h"

#include <cstdint>
#include <functional>
#include <string>

// A stand-in SkyrimNet.dll.
//
// Built under the real name and dropped beside the test executable, so the
// upstream header's LoadLibraryA + GetProcAddress resolution runs unchanged.
// See FakeSkyrimNet.h for why this is a whole DLL rather than a seam.
//
// Only the exports NarrativeEngine actually calls are implemented. Anything
// else stays unresolved, exactly as it would against a SkyrimNet that did not
// have it — which is the state a caller has to survive.

// Only ever a pointer in these signatures, so a forward declaration is enough
// and keeps CommonLibSSE out of this DLL entirely.
namespace RE
{
    class Actor;
}

namespace
{
    using NarrativeEngine::Testing::FakeSkyrimNetState;

    FakeSkyrimNetState& State()
    {
        static FakeSkyrimNetState state;
        return state;
    }

    void CopyInto(char* dest, std::size_t capacity, const char* src)
    {
        if (!dest || capacity == 0)
            return;
        if (!src) {
            dest[0] = '\0';
            return;
        }
        std::size_t i = 0;
        for (; i + 1 < capacity && src[i] != '\0'; ++i)
            dest[i] = src[i];
        dest[i] = '\0';
    }
} // namespace

extern "C"
{

    __declspec(dllexport) int PublicGetVersion()
    {
        auto& s = State();
        ++s.versionQueries;
        return s.version;
    }

    __declspec(dllexport) bool PublicIsMemorySystemReady()
    {
        return State().memorySystemReady;
    }

    __declspec(dllexport) std::uint64_t PublicFormIDToUUID(std::uint32_t formId)
    {
        auto& s = State();
        s.lastFormID = formId;
        return s.uuidAnswer;
    }

    __declspec(dllexport) std::string PublicGetRecentEvents(std::uint32_t formId,
                                                            int maxCount,
                                                            const char* eventTypeFilter)
    {
        auto& s = State();
        ++s.recentEventsCalls;
        s.lastFormID = formId;
        s.lastMaxCount = maxCount;
        CopyInto(s.lastEventFilter, sizeof(s.lastEventFilter), eventTypeFilter);
        return s.eventsJson;
    }

    __declspec(dllexport) std::string PublicGetMemoriesForActor(std::uint32_t formId,
                                                                int maxCount,
                                                                const char* contextQuery)
    {
        auto& s = State();
        ++s.memoriesForActorCalls;
        s.lastFormID = formId;
        s.lastMaxCount = maxCount;
        CopyInto(s.lastContextQuery, sizeof(s.lastContextQuery), contextQuery);
        return s.memoriesJson;
    }

    __declspec(dllexport) std::string PublicQueryMemoriesForActor(std::uint32_t formId, const char* queryJSON)
    {
        auto& s = State();
        ++s.queryMemoriesCalls;
        s.lastFormID = formId;
        CopyInto(s.lastQueryJson, sizeof(s.lastQueryJson), queryJSON);
        return s.queryMemoriesJson;
    }

    __declspec(dllexport) std::string PublicGetRecentDialogue(std::uint32_t formId, int maxExchanges)
    {
        auto& s = State();
        ++s.recentDialogueCalls;
        s.lastFormID = formId;
        s.lastMaxCount = maxExchanges;
        return s.dialogueJson;
    }

    __declspec(dllexport) std::string PublicGetActorEngagement(int maxCount,
                                                               bool excludePlayer,
                                                               bool playerEventsOnly,
                                                               double shortWindowSeconds,
                                                               double mediumWindowSeconds)
    {
        auto& s = State();
        ++s.engagementCalls;
        s.lastMaxCount = maxCount;
        s.lastExcludePlayer = excludePlayer;
        s.lastPlayerEventsOnly = playerEventsOnly;
        s.lastShortWindow = shortWindowSeconds;
        s.lastMediumWindow = mediumWindowSeconds;
        return s.engagementJson;
    }

    __declspec(dllexport) bool PublicHasDecorator(const char* name)
    {
        auto& s = State();
        CopyInto(s.lastDecoratorName, sizeof(s.lastDecoratorName), name);
        return s.hasDecoratorAnswer;
    }

    __declspec(dllexport) bool PublicRegisterDecorator(const char* name,
                                                       const char* description,
                                                       std::function<std::string(RE::Actor*)> callback)
    {
        auto& s = State();
        ++s.registerDecoratorCalls;
        CopyInto(s.lastDecoratorName, sizeof(s.lastDecoratorName), name);
        CopyInto(s.lastDecoratorDescription, sizeof(s.lastDecoratorDescription), description);
        // Call it once with a null actor, the way SkyrimNet's prompt renderer
        // eventually will. Decorators are documented to ignore the actor, so a
        // decorator that dereferenced it would fault here rather than in game --
        // and what it returned is recorded, because the callbacks are otherwise
        // unreachable from a test.
        if (callback && s.recordedDecorators < FakeSkyrimNetState::kMaxRecordedDecorators) {
            const int slot = s.recordedDecorators++;
            CopyInto(s.decoratorNames[slot], sizeof(s.decoratorNames[slot]), name);
            const std::string result = callback(nullptr);
            CopyInto(s.decoratorResults[slot], sizeof(s.decoratorResults[slot]), result.c_str());
        }
        return s.registerDecoratorSucceeds;
    }

    __declspec(dllexport) bool PublicSendCustomPromptToLLM(
        const char* promptName,
        const char* variant,
        const char* contextJson,
        std::function<void(const char* response, int success)> callback)
    {
        auto& s = State();
        ++s.sendPromptCalls;
        CopyInto(s.lastPromptName, sizeof(s.lastPromptName), promptName);
        CopyInto(s.lastVariant, sizeof(s.lastVariant), variant);
        CopyInto(s.lastContextJson, sizeof(s.lastContextJson), contextJson);
        if (!s.sendPromptAccepts) {
            return false;
        }
        // Delivered inline rather than from another thread. What the wrapper has to
        // get right is that it never runs the caller's callback on whatever thread
        // SkyrimNet delivered on, and calling straight back from here is the
        // sharpest version of that: a wrapper that invoked the callback directly
        // would run it on the caller's own thread and pass.
        if (callback) {
            callback(s.promptResponse, s.sendPromptSucceeds ? 1 : 0);
        }
        return true;
    }

    __declspec(dllexport) int PublicAddMemory(std::uint32_t formId,
                                              const char* contentText,
                                              float importance,
                                              const char* memoryType,
                                              const char* emotion,
                                              const char* location,
                                              const char* tagsJSON,
                                              const char* relatedActorsJSON)
    {
        auto& s = State();
        ++s.addMemoryCalls;
        s.lastFormID = formId;
        s.lastImportance = importance;
        CopyInto(s.lastMemoryText, sizeof(s.lastMemoryText), contentText);
        CopyInto(s.lastMemoryType, sizeof(s.lastMemoryType), memoryType);
        CopyInto(s.lastEmotion, sizeof(s.lastEmotion), emotion);
        CopyInto(s.lastLocation, sizeof(s.lastLocation), location);
        // Null and empty are different things here: SkyrimNet takes its documented
        // "no tags" path only on null, and tries to parse "" as JSON otherwise.
        s.lastTagsWereNull = tagsJSON == nullptr;
        s.lastRelatedActorsWereNull = relatedActorsJSON == nullptr;
        CopyInto(s.lastTagsJson, sizeof(s.lastTagsJson), tagsJSON);
        CopyInto(s.lastRelatedActorsJson, sizeof(s.lastRelatedActorsJson), relatedActorsJSON);
        return s.addMemoryAnswer;
    }

    // How the test reaches the fake's state. Not part of SkyrimNet's API.
    __declspec(dllexport) NarrativeEngine::Testing::FakeSkyrimNetState* NarrativeEngineFakeSkyrimNetState()
    {
        return &State();
    }

} // extern "C"
