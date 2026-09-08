#include "FakePrismaUI.h"

#include <PrismaUI_API.h> // upstream

#include <cstring>

// A stand-in PrismaUI.dll.
//
// Built under the real name and dropped beside the test executable, so the
// wrapper's GetModuleHandle / GetProcAddress handshake runs unchanged. See
// FakePrismaUI.h for why this is a whole DLL rather than a seam in the wrapper.
//
// Nothing here allocates on behalf of the caller: the two binaries have
// separate heaps, so every string the interface is handed is copied into a
// fixed buffer that this module owns.

namespace
{
    using NarrativeEngine::Testing::FakePrismaState;

    FakePrismaState& State()
    {
        static FakePrismaState state;
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

    // Implements every pure virtual of IVPrismaUI1. Most are recorded rather
    // than acted on; the handful the wrapper reads answers from are steerable
    // through the shared state.
    class FakePrismaUI final : public PRISMA_UI_API::IVPrismaUI1
    {
    public:
        PrismaView CreateView(const char* htmlPath, PRISMA_UI_API::OnDomReadyCallback) noexcept override
        {
            auto& s = State();
            ++s.createViewCalls;
            CopyInto(s.lastHtmlPath, sizeof(s.lastHtmlPath), htmlPath);
            if (s.createViewFailsWith != 0)
                return 0;
            s.lastView = s.nextView++;
            return s.lastView;
        }

        void Invoke(PrismaView, const char*, PRISMA_UI_API::JSCallback) noexcept override {}

        void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept override
        {
            auto& s = State();
            ++s.interopCalls;
            s.lastView = view;
            CopyInto(s.lastFunctionName, sizeof(s.lastFunctionName), functionName);
            CopyInto(s.lastArgument, sizeof(s.lastArgument), argument);
        }

        void RegisterJSListener(PrismaView view,
                                const char* functionName,
                                PRISMA_UI_API::JSListenerCallback) noexcept override
        {
            auto& s = State();
            ++s.registerListenerCalls;
            s.lastView = view;
            CopyInto(s.lastFunctionName, sizeof(s.lastFunctionName), functionName);
        }

        bool HasFocus(PrismaView) noexcept override
        {
            return false;
        }

        bool Focus(PrismaView view, bool pauseGame, bool disableFocusMenu) noexcept override
        {
            auto& s = State();
            ++s.focusCalls;
            s.lastView = view;
            s.lastPauseGame = pauseGame;
            s.lastDisableFocusMenu = disableFocusMenu;
            return true;
        }

        void Unfocus(PrismaView view) noexcept override
        {
            auto& s = State();
            ++s.unfocusCalls;
            s.lastView = view;
        }

        void Show(PrismaView view) noexcept override
        {
            auto& s = State();
            ++s.showCalls;
            s.lastView = view;
        }

        void Hide(PrismaView view) noexcept override
        {
            auto& s = State();
            ++s.hideCalls;
            s.lastView = view;
        }

        bool IsHidden(PrismaView view) noexcept override
        {
            State().lastView = view;
            return State().isHiddenAnswer;
        }

        int GetScrollingPixelSize(PrismaView) noexcept override
        {
            return 0;
        }
        void SetScrollingPixelSize(PrismaView, int) noexcept override {}

        bool IsValid(PrismaView view) noexcept override
        {
            State().lastView = view;
            return State().isValidAnswer;
        }

        void Destroy(PrismaView view) noexcept override
        {
            auto& s = State();
            ++s.destroyCalls;
            s.lastView = view;
        }

        void SetOrder(PrismaView, int) noexcept override {}
        int GetOrder(PrismaView) noexcept override
        {
            return 0;
        }
        void CreateInspectorView(PrismaView) noexcept override {}
        void SetInspectorVisibility(PrismaView, bool) noexcept override {}
        bool IsInspectorVisible(PrismaView) noexcept override
        {
            return false;
        }
        void SetInspectorBounds(PrismaView, float, float, unsigned int, unsigned int) noexcept override {}

        bool HasAnyActiveFocus() noexcept override
        {
            return State().hasAnyActiveFocusAnswer;
        }
    };

    FakePrismaUI& Interface()
    {
        static FakePrismaUI instance;
        return instance;
    }
} // namespace

// The export the upstream header resolves by undecorated name.
extern "C" __declspec(dllexport) void* RequestPluginAPI(PRISMA_UI_API::InterfaceVersion a_interfaceVersion)
{
    auto& s = State();
    ++s.interfaceRequests;
    s.lastRequestedVersion = static_cast<std::uint8_t>(a_interfaceVersion);
    if (s.refuseInterface)
        return nullptr;
    // Only V1 is implemented, which is all the wrapper asks for. A request for
    // anything else is refused the way an older PrismaUI would refuse it.
    if (a_interfaceVersion != PRISMA_UI_API::InterfaceVersion::V1)
        return nullptr;
    return static_cast<PRISMA_UI_API::IVPrismaUI1*>(&Interface());
}

// How the test reaches the fake's state. Not part of PrismaUI's API.
extern "C" __declspec(dllexport) NarrativeEngine::Testing::FakePrismaState* NarrativeEngineFakePrismaState()
{
    return &State();
}
