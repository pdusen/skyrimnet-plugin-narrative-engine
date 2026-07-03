#include <PrismaUI.h>

// The upstream PrismaUI header reaches us via PRISMA_UI_INCLUDE (set in
// CMakeLists.txt). It declares the PRISMA_UI_API namespace plus the inline
// RequestPluginAPI() helper that does the GetModuleHandle / GetProcAddress
// dance against PrismaUI.dll. Only this TU includes it.
#include <PrismaUI_API.h>  // upstream — distinct from our own <PrismaUI.h>

#include <logger.h>

namespace NarrativeEngine::PrismaUI_API
{
    namespace
    {
        bool g_initialized = false;
        PRISMA_UI_API::IVPrismaUI1* g_api = nullptr;
    }

    bool Initialize()
    {
        if (g_initialized) {
            return g_api != nullptr;
        }
        g_initialized = true;

        // PrismaUI is loaded by SKSE before kDataLoaded if it's installed, so
        // RequestPluginAPI's internal GetModuleHandle finds it without us
        // bumping the refcount via LoadLibrary ourselves.
        g_api = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();

        if (g_api) {
            logger::info("PrismaUI: loaded");
        } else {
            // Info-level (not error) — PrismaUI is a runtime soft dependency.
            logger::info("PrismaUI: not found; dashboard disabled");
        }
        return g_api != nullptr;
    }

    bool IsAvailable()
    {
        return g_api != nullptr;
    }

    // ---- View management ---------------------------------------------------
    // Each wrapper maps 1:1 to an IVPrismaUI1 virtual; the comment names the
    // exact upstream method so future PrismaUI API drift is easy to trace.

    ViewHandle CreateView(const std::string& htmlPath)
    {
        // IVPrismaUI1::CreateView(const char* htmlPath, OnDomReadyCallback = nullptr)
        if (!g_api) {
            return kInvalidView;
        }
        return g_api->CreateView(htmlPath.c_str(), nullptr);
    }

    void Destroy(ViewHandle view)
    {
        // IVPrismaUI1::Destroy(PrismaView)
        if (!g_api || view == kInvalidView) {
            return;
        }
        g_api->Destroy(view);
    }

    bool IsValid(ViewHandle view)
    {
        // IVPrismaUI1::IsValid(PrismaView)
        if (!g_api || view == kInvalidView) {
            return false;
        }
        return g_api->IsValid(view);
    }

    void Show(ViewHandle view)
    {
        // IVPrismaUI1::Show(PrismaView)
        if (!g_api || view == kInvalidView) {
            return;
        }
        g_api->Show(view);
    }

    void Hide(ViewHandle view)
    {
        // IVPrismaUI1::Hide(PrismaView)
        if (!g_api || view == kInvalidView) {
            return;
        }
        g_api->Hide(view);
    }

    void Focus(ViewHandle view, bool pauseGame, bool disableFocusMenu)
    {
        // IVPrismaUI1::Focus(PrismaView, bool pauseGame, bool disableFocusMenu)
        if (!g_api || view == kInvalidView) {
            return;
        }
        g_api->Focus(view, pauseGame, disableFocusMenu);
    }

    void Unfocus(ViewHandle view)
    {
        // IVPrismaUI1::Unfocus(PrismaView)
        if (!g_api || view == kInvalidView) {
            return;
        }
        g_api->Unfocus(view);
    }

    bool IsHidden(ViewHandle view)
    {
        // IVPrismaUI1::IsHidden(PrismaView)
        // When the view doesn't exist, treating it as "hidden" matches
        // dashboard-toggle expectations (toggling a missing view should not
        // appear visible).
        if (!g_api || view == kInvalidView) {
            return true;
        }
        return g_api->IsHidden(view);
    }

    void InvokeJS(ViewHandle view, const std::string& functionName, const std::string& argument)
    {
        // IVPrismaUI1::InteropCall(PrismaView, const char* functionName, const char* argument)
        // PrismaUI's "InteropCall" is the fast path for invoking a globally-
        // exposed JS function with a single string argument — what our
        // dashboard's `window.updateFullState(jsonString)` expects.
        if (!g_api || view == kInvalidView) {
            return;
        }
        g_api->InteropCall(view, functionName.c_str(), argument.c_str());
    }

    void RegisterJSListener(ViewHandle view, const std::string& functionName, JSListenerCallback callback)
    {
        // IVPrismaUI1::RegisterJSListener(PrismaView, const char* functionName, JSListenerCallback)
        // Our wrapper's JSListenerCallback typedef is ABI-compatible with
        // PRISMA_UI_API::JSListenerCallback (same signature); reinterpret is
        // used to keep upstream headers out of PrismaUI.h.
        if (!g_api || view == kInvalidView || !callback) {
            return;
        }
        g_api->RegisterJSListener(
            view,
            functionName.c_str(),
            reinterpret_cast<PRISMA_UI_API::JSListenerCallback>(callback));
    }
}
