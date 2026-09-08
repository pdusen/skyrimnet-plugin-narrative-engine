#pragma once

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

        // What the query endpoints hand back.
        char eventsJson[512] = "[]";
        char memoriesJson[512] = "[]";
        char queryMemoriesJson[512] = "[]";
        char dialogueJson[512] = "[]";
        char engagementJson[512] = "[]";
        char promptResponse[512] = "the response";

        // Clears the per-call record. Leaves `version` and `versionQueries`
        // alone: FindFunctions runs once per process and its record is evidence
        // no later case can recreate.
        void Reset()
        {
            const int keptVersion = version;
            const int keptQueries = versionQueries;
            *this = FakeSkyrimNetState{};
            version = keptVersion;
            versionQueries = keptQueries;
        }
    };

    using FakeSkyrimNetStateFunc = FakeSkyrimNetState* (*)();
    inline constexpr const char* kFakeSkyrimNetStateExport = "NarrativeEngineFakeSkyrimNetState";
} // namespace NarrativeEngine::Testing
