#include "FakePo3Tweaks.h"

#include <cstdint>

// A stand-in po3_Tweaks.dll.
//
// Built under the real name and dropped beside the test executable, so the
// sender pool's GetModuleHandle / GetProcAddress lookup runs unchanged. See
// FakePo3Tweaks.h for why this is a whole DLL.

namespace
{
    using NarrativeEngine::Testing::FakePo3TweaksState;

    FakePo3TweaksState& State()
    {
        static FakePo3TweaksState state;
        return state;
    }
} // namespace

extern "C"
{
    // Same signature and miss behaviour as the real export: a C string the DLL
    // owns, and an empty one when the FormID was never cached.
    __declspec(dllexport) const char* GetFormEditorID(std::uint32_t formID)
    {
        auto& s = State();
        if (s.lookups < FakePo3TweaksState::kMaxRecordedLookups) {
            s.askedFor[s.lookups] = formID;
        }
        ++s.lookups;
        for (int i = 0; i < s.entries; ++i) {
            if (s.formIDs[i] == formID) {
                return s.editorIDs[i];
            }
        }
        return "";
    }

    __declspec(dllexport) FakePo3TweaksState* NarrativeEngineFakePo3TweaksState()
    {
        return &State();
    }
} // extern "C"
