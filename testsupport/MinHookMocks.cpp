#include "MinHookMocks.h"

#include <MinHook.h>

// See MinHookMocks.h for why the detours are recorded rather than installed.

namespace NarrativeEngine::Testing
{
    void MinHookState::Reset()
    {
        initializeCalls = 0;
        initializeSucceeds = true;
        createSucceeds = true;
        enableSucceeds = true;
        hooks.clear();
    }

    MinHookState& MinHookMocks()
    {
        static MinHookState state;
        return state;
    }
} // namespace NarrativeEngine::Testing

extern "C"
{

    MH_STATUS WINAPI MH_Initialize(VOID)
    {
        auto& state = NarrativeEngine::Testing::MinHookMocks();
        ++state.initializeCalls;
        if (!state.initializeSucceeds) {
            return MH_ERROR_MEMORY_ALLOC;
        }
        // The real library answers this on every call after the first, and
        // production treats it as success -- so reproducing it is the only way
        // that branch is ever taken.
        return state.initializeCalls == 1 ? MH_OK : MH_ERROR_ALREADY_INITIALIZED;
    }

    MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal)
    {
        auto& state = NarrativeEngine::Testing::MinHookMocks();
        if (!state.createSucceeds) {
            return MH_ERROR_UNSUPPORTED_FUNCTION;
        }
        state.hooks.push_back({pTarget, pDetour, false});
        // The trampoline the detour would chain to. There is no original
        // instruction stream to preserve here, so the detour itself stands in for
        // it: anything that calls through lands somewhere real rather than at
        // null, and nothing in a test ever calls through.
        if (ppOriginal) {
            *ppOriginal = pDetour;
        }
        return MH_OK;
    }

    MH_STATUS WINAPI MH_EnableHook(LPVOID pTarget)
    {
        auto& state = NarrativeEngine::Testing::MinHookMocks();
        if (!state.enableSucceeds) {
            return MH_ERROR_NOT_CREATED;
        }
        for (auto& hook : state.hooks) {
            if (hook.target == pTarget) {
                hook.enabled = true;
            }
        }
        return MH_OK;
    }

} // extern "C"
