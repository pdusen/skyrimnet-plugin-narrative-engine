#pragma once

#include <cstdint>

// Shared state between the fake PrismaUI.dll and the test that drives it.
//
// PrismaUI is a runtime soft dependency: our wrapper finds it with
// GetModuleHandle("PrismaUI.dll") and GetProcAddress("RequestPluginAPI"), and
// there is no seam between those calls and the wrapper. So rather than pretend
// there is one, the harness builds a real DLL of that name and lets the wrapper
// find it exactly as it would in game. That exercises the interface-request
// handshake itself, which is the part most likely to break when PrismaUI
// changes, and it is the same shape SkyrimNet's API needs.
//
// The DLL owns the state; the test reaches it through a second export. Passing
// a plain struct across the boundary keeps the two sides free of any shared
// runtime — they are separate binaries with separate heaps, so nothing here may
// own memory.
namespace NarrativeEngine::Testing
{
    struct FakePrismaState
    {
        // What the interface request should answer with. `refuseInterface`
        // reproduces an installed PrismaUI too old for the version we ask for,
        // which is a real state and the one users hit after a partial update.
        bool refuseInterface = false;
        int interfaceRequests = 0;
        std::uint8_t lastRequestedVersion = 0xFF;

        // Views. The fake hands out increasing handles and remembers the last
        // one it was asked about, so a wrapper that passed the wrong handle
        // through would be visible.
        std::uint64_t nextView = 1;
        std::uint64_t createViewFailsWith = 0; // non-zero forces CreateView to fail
        std::uint64_t lastView = 0;

        int createViewCalls = 0;
        int destroyCalls = 0;
        int showCalls = 0;
        int hideCalls = 0;
        int focusCalls = 0;
        int unfocusCalls = 0;
        int interopCalls = 0;
        int registerListenerCalls = 0;

        // Every listener the view registered, by name and by function
        // pointer. Retained because the dashboard's controls exist ONLY as
        // these callbacks -- the page calls them and nothing else does, so a
        // test that wants to press a button has to be handed the button.
        //
        // A raw function pointer rather than anything richer: the two sides
        // are separate binaries with separate heaps, and this struct crosses
        // between them.
        using ListenerFn = void (*)(const char* argument);
        static constexpr int kMaxRecordedListeners = 64;
        int recordedListeners = 0;
        char listenerNames[kMaxRecordedListeners][64]{};
        ListenerFn listenerCallbacks[kMaxRecordedListeners]{};

        bool lastPauseGame = false;
        bool lastDisableFocusMenu = false;

        bool isValidAnswer = true;
        bool isHiddenAnswer = false;
        bool hasAnyActiveFocusAnswer = false;

        // Last strings seen, copied into fixed buffers so neither side has to
        // free anything across the module boundary.
        char lastHtmlPath[260]{};
        char lastFunctionName[128]{};
        // Sized for a whole dashboard state blob rather than a token one: the
        // page is handed the entire Director state in a single string, and a
        // buffer that truncated it would hand a test unparseable JSON and
        // look exactly like a compose that produced it.
        char lastArgument[32768]{};

        // Clears the per-call record, and deliberately NOT the interface
        // handshake above it. The wrapper performs that handshake exactly once
        // per process and caches the result, so its record is evidence no later
        // case can recreate — a blanket reset would erase the only proof of
        // which interface version was asked for.
        void Reset()
        {
            const bool refuse = refuseInterface;
            const int requests = interfaceRequests;
            const std::uint8_t version = lastRequestedVersion;
            *this = FakePrismaState{};
            refuseInterface = refuse;
            interfaceRequests = requests;
            lastRequestedVersion = version;
        }
    };

    // Exported by the fake DLL under this name; the test resolves it with
    // GetProcAddress and drives the fake through the pointer it returns.
    using FakePrismaStateFunc = FakePrismaState* (*)();
    inline constexpr const char* kFakePrismaStateExport = "NarrativeEngineFakePrismaState";
} // namespace NarrativeEngine::Testing
