#pragma once

#include <vector>

// Stand-in for MinHook, the detour library the letter pool patches two engine
// functions with.
//
// Nothing here installs anything. A detour needs a real function at a real
// address to overwrite, and the harness's "addresses" are stand-ins reached
// through the address library -- there is no prologue to rewrite and no
// trampoline to build. What the code under test is responsible for is asking
// for the right hooks, in the right order, once, and coping when the library
// refuses; all four of those are visible from the calls alone.
//
// Beside the harness rather than in EngineMock because MinHook is not the
// engine: it is an ordinary third-party library the plugin links, and the
// plugin would still call it if Skyrim's own API never changed.
namespace NarrativeEngine::Testing
{
    struct MinHookState
    {
        int initializeCalls = 0;

        // What MH_Initialize answers. The real one reports
        // MH_ERROR_ALREADY_INITIALIZED on a second call, which production
        // treats as success -- so that is not a failure a test needs, but a
        // genuine refusal is.
        bool initializeSucceeds = true;

        // What MH_CreateHook and MH_EnableHook answer. Either can refuse
        // independently, and production has to log and carry on rather than
        // leave a half-installed hook.
        bool createSucceeds = true;
        bool enableSucceeds = true;

        struct Hook
        {
            const void* target = nullptr;
            const void* detour = nullptr;
            bool enabled = false;
        };

        // Every hook asked for, in order. A pool that installs one hook twice
        // has doubled the engine work on every book the player opens.
        std::vector<Hook> hooks;

        void Reset();
    };

    MinHookState& MinHookMocks();
} // namespace NarrativeEngine::Testing
