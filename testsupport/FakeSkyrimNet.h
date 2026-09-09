#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Shared state between the fake SkyrimNet.dll and the test that drives it.
//
// SkyrimNet, like PrismaUI, is found at runtime — its header's FindFunctions()
// does LoadLibraryA("SkyrimNet") and then a GetProcAddress per export, inline,
// with no seam in between. So the harness builds a real DLL under that name
// beside the test executable and lets the wrapper resolve against it. That
// covers the version gate as well as the calls: the header only resolves an
// export when the version it reports is high enough, so a fake that reports an
// old version reproduces a genuinely older install.
//
// The DLL owns the state; the test reaches it through a second export. Nothing
// here may own memory — the two binaries have separate heaps.
//
// One thing this fake deliberately does NOT reproduce: SkyrimNet returns
// std::string by value across the DLL boundary, which only works because both
// modules use the same dynamically-linked CRT. That is a real constraint of the
// upstream API rather than something the test invents, and the fake is built
// with the same settings for the same reason the plugin must be.
namespace NarrativeEngine::Testing
{
    struct FakeSkyrimNetState
    {
        // What FindFunctions sees. The header gates each export behind the
        // version, so lowering this reproduces an older SkyrimNet: the newer
        // pointers stay null and the wrapper has to degrade rather than crash.
        int version = 10;
        int versionQueries = 0;

        bool memorySystemReady = true;
        bool hasDecoratorAnswer = false;
        bool registerDecoratorSucceeds = true;
        bool sendPromptAccepts = true;
        bool sendPromptSucceeds = true;

        // While set, a prompt is accepted and then never answered until it is
        // cleared. That is what a model still thinking looks like, and it is
        // the only way for a test to hold a caller's in-flight latch without
        // a real round trip to hold it for them. Read from the fake's own
        // thread-agnostic loop, so the test clearing it must do so from
        // another thread than the one that is waiting.
        std::atomic<bool> holdPromptAnswer{false};

        std::uint64_t uuidAnswer = 0x00ABCDEF12345678ull;
        int addMemoryAnswer = 77;

        int registerDecoratorCalls = 0;
        int sendPromptCalls = 0;
        int addMemoryCalls = 0;
        int recentEventsCalls = 0;
        int memoriesForActorCalls = 0;
        int queryMemoriesCalls = 0;
        int recentDialogueCalls = 0;
        int engagementCalls = 0;

        // Arguments last seen. Fixed buffers so neither module frees the
        // other's memory.
        std::uint32_t lastFormID = 0;
        int lastMaxCount = 0;
        float lastImportance = 0.0f;
        bool lastExcludePlayer = false;
        bool lastPlayerEventsOnly = false;
        double lastShortWindow = 0.0;
        double lastMediumWindow = 0.0;

        char lastPromptName[128]{};
        char lastVariant[128]{};
        char lastContextJson[1024]{};
        char lastEventFilter[128]{};
        char lastContextQuery[256]{};
        char lastQueryJson[1024]{};
        char lastDecoratorName[128]{};
        char lastDecoratorDescription[256]{};

        // Every decorator registered this session, with what its callback
        // answered when the fake called it. Recorded per registration rather
        // than as a "last" pair because a single Register() call installs
        // several, and each one's value is a separate contract with the prompt
        // template that renders it. The callbacks are anonymous-namespace
        // statics, so what SkyrimNet was handed is the only way to reach them.
        static constexpr int kMaxRecordedDecorators = 8;
        int recordedDecorators = 0;
        char decoratorNames[kMaxRecordedDecorators][128]{};
        char decoratorResults[kMaxRecordedDecorators][256]{};
        char lastMemoryText[512]{};
        char lastMemoryType[64]{};
        char lastEmotion[64]{};
        char lastLocation[128]{};
        // Empty here means the wrapper passed nullptr, which is the documented
        // "no tags" path and is not the same as passing "".
        bool lastTagsWereNull = false;
        bool lastRelatedActorsWereNull = false;
        char lastTagsJson[256]{};
        char lastRelatedActorsJson[256]{};

        // What the query endpoints hand back. Sized for a realistic answer
        // rather than a token one: SkyrimNet returns tens of rows per call, and
        // a buffer that truncated one would hand the module unparseable JSON
        // and look exactly like a query that found nothing.
        static constexpr std::size_t kAnswerCapacity = 8192;
        char eventsJson[kAnswerCapacity] = "[]";
        char memoriesJson[kAnswerCapacity] = "[]";
        char queryMemoriesJson[kAnswerCapacity] = "[]";
        char dialogueJson[kAnswerCapacity] = "[]";
        char engagementJson[kAnswerCapacity] = "[]";
        char promptResponse[1024] = "the response";

        // Clears the per-call record. Leaves `version` and `versionQueries`
        // alone: FindFunctions runs once per process and its record is evidence
        // no later case can recreate.
        void Reset()
        {
            const int keptVersion = version;
            const int keptQueries = versionQueries;
            // Field by field rather than by assignment: the hold flag is an
            // atomic and the whole struct is no longer copyable.
            FakeSkyrimNetState fresh;
            version = keptVersion;
            versionQueries = keptQueries;
            memorySystemReady = fresh.memorySystemReady;
            hasDecoratorAnswer = fresh.hasDecoratorAnswer;
            registerDecoratorSucceeds = fresh.registerDecoratorSucceeds;
            sendPromptAccepts = fresh.sendPromptAccepts;
            sendPromptSucceeds = fresh.sendPromptSucceeds;
            holdPromptAnswer.store(false);
            uuidAnswer = fresh.uuidAnswer;
            addMemoryAnswer = fresh.addMemoryAnswer;
            registerDecoratorCalls = 0;
            sendPromptCalls = 0;
            addMemoryCalls = 0;
            recentEventsCalls = 0;
            memoriesForActorCalls = 0;
            queryMemoriesCalls = 0;
            recentDialogueCalls = 0;
            engagementCalls = 0;
            lastFormID = 0;
            lastMaxCount = 0;
            lastImportance = 0.0f;
            lastExcludePlayer = false;
            lastPlayerEventsOnly = false;
            lastShortWindow = 0.0;
            lastMediumWindow = 0.0;
            lastPromptName[0] = '\0';
            lastVariant[0] = '\0';
            lastContextJson[0] = '\0';
            lastEventFilter[0] = '\0';
            lastContextQuery[0] = '\0';
            lastQueryJson[0] = '\0';
            lastDecoratorName[0] = '\0';
            lastDecoratorDescription[0] = '\0';
            recordedDecorators = 0;
            decoratorNames[0][0] = '\0';
            decoratorResults[0][0] = '\0';
            lastMemoryText[0] = '\0';
            lastMemoryType[0] = '\0';
            lastEmotion[0] = '\0';
            lastLocation[0] = '\0';
            lastTagsWereNull = false;
            lastRelatedActorsWereNull = false;
            lastTagsJson[0] = '\0';
            lastRelatedActorsJson[0] = '\0';
            CopyLiteral(eventsJson, "[]");
            CopyLiteral(memoriesJson, "[]");
            CopyLiteral(queryMemoriesJson, "[]");
            CopyLiteral(dialogueJson, "[]");
            CopyLiteral(engagementJson, "[]");
            CopyLiteral(promptResponse, "the response");
        }

    private:
        template <std::size_t N> static void CopyLiteral(char (&dest)[N], const char* text)
        {
            std::size_t i = 0;
            for (; i + 1 < N && text[i] != '\0'; ++i)
                dest[i] = text[i];
            dest[i] = '\0';
        }
    };

    using FakeSkyrimNetStateFunc = FakeSkyrimNetState* (*)();
    inline constexpr const char* kFakeSkyrimNetStateExport = "NarrativeEngineFakeSkyrimNetState";
} // namespace NarrativeEngine::Testing
