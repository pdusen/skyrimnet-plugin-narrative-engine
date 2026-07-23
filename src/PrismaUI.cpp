#include <PrismaUI.h>

// The upstream PrismaUI header reaches us via PRISMA_UI_INCLUDE (set in
// CMakeLists.txt). It declares the PRISMA_UI_API namespace plus the inline
// RequestPluginAPI() helper that does the GetModuleHandle / GetProcAddress
// dance against PrismaUI.dll. Only this TU includes it.
#include <PrismaUI_API.h> // upstream — distinct from our own <PrismaUI.h>

#include <logger.h>

#include <Windows.h>

#include <filesystem>
#include <system_error>

namespace NarrativeEngine::PrismaUI_API
{
    namespace
    {
        bool g_initialized = false;
        PRISMA_UI_API::IVPrismaUI1* g_api = nullptr;

        // Diagnostic helper: reports whether PrismaUI.dll is loaded and,
        // if so, where it was loaded from. Uses the same GetModuleHandle
        // path RequestPluginAPI does internally so a divergence between
        // "SKSE loaded the dll" and "our probe finds it" would show up.
        void TraceDllPresence()
        {
            HMODULE mod = ::GetModuleHandleA("PrismaUI.dll");
            if (!mod) {
                logger::trace("PrismaUI[trace]: GetModuleHandle('PrismaUI.dll') -> NULL "
                              "(SKSE has not loaded the DLL — soft dependency absent)");
                return;
            }
            char path[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameA(mod, path, static_cast<DWORD>(sizeof(path)));
            logger::trace("PrismaUI[trace]: PrismaUI.dll loaded at {} (path='{}')",
                          static_cast<const void*>(mod),
                          n ? std::string{path, path + n} : std::string{"<GetModuleFileName failed>"});
        }
    } // namespace

    bool Initialize()
    {
        if (g_initialized) {
            logger::trace("PrismaUI[trace]: Initialize re-entered; returning cached g_api={}",
                          static_cast<const void*>(g_api));
            return g_api != nullptr;
        }
        g_initialized = true;

        TraceDllPresence();

        // PrismaUI is loaded by SKSE before kDataLoaded if it's installed, so
        // RequestPluginAPI's internal GetModuleHandle finds it without us
        // bumping the refcount via LoadLibrary ourselves.
        g_api = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();

        if (g_api) {
            logger::info("PrismaUI: loaded");
            logger::trace("PrismaUI[trace]: IVPrismaUI1 interface obtained at {}", static_cast<const void*>(g_api));
        } else {
            // Info-level (not error) — PrismaUI is a runtime soft dependency.
            logger::info("PrismaUI: not found; dashboard disabled");
            logger::trace("PrismaUI[trace]: RequestPluginAPI returned nullptr — "
                          "either PrismaUI.dll is not present or its exported query rejected the version request");
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
            logger::trace("PrismaUI[trace]: CreateView('{}') rejected — g_api is null", htmlPath);
            return kInvalidView;
        }
        // Sanity-check that the HTML file the caller passed is actually
        // present under Data/PrismaUI/views/ before we hand it to
        // PrismaUI's DLL. PrismaUI resolves the path relative to that
        // root (see kHtmlPath in DashboardUIManager.cpp) — the trace
        // walks the same rules so the log tells you exactly what was
        // looked up and whether it existed.
        std::error_code ec;
        const std::filesystem::path viewsRoot{"Data/PrismaUI/views"};
        const auto full = viewsRoot / htmlPath;
        const auto abs = std::filesystem::absolute(full, ec);
        const bool present = !ec && std::filesystem::exists(abs, ec);
        logger::trace("PrismaUI[trace]: CreateView probing html '{}' -> resolved abs='{}' exists={}",
                      htmlPath,
                      ec ? full.string() : abs.string(),
                      present ? 1 : 0);
        const ViewHandle handle = g_api->CreateView(htmlPath.c_str(), nullptr);
        logger::trace("PrismaUI[trace]: CreateView('{}') returned handle=0x{:X} ({})",
                      htmlPath,
                      static_cast<std::uint64_t>(handle),
                      handle == kInvalidView ? "INVALID" : "ok");
        return handle;
    }

    void Destroy(ViewHandle view)
    {
        // IVPrismaUI1::Destroy(PrismaView)
        if (!g_api || view == kInvalidView) {
            return;
        }
        logger::trace("PrismaUI[trace]: Destroy(handle=0x{:X})", static_cast<std::uint64_t>(view));
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
            logger::trace("PrismaUI[trace]: Show rejected (api={} view=0x{:X})",
                          static_cast<const void*>(g_api),
                          static_cast<std::uint64_t>(view));
            return;
        }
        logger::trace("PrismaUI[trace]: Show(handle=0x{:X})", static_cast<std::uint64_t>(view));
        g_api->Show(view);
    }

    void Hide(ViewHandle view)
    {
        // IVPrismaUI1::Hide(PrismaView)
        if (!g_api || view == kInvalidView) {
            logger::trace("PrismaUI[trace]: Hide rejected (api={} view=0x{:X})",
                          static_cast<const void*>(g_api),
                          static_cast<std::uint64_t>(view));
            return;
        }
        logger::trace("PrismaUI[trace]: Hide(handle=0x{:X})", static_cast<std::uint64_t>(view));
        g_api->Hide(view);
    }

    void Focus(ViewHandle view, bool pauseGame, bool disableFocusMenu)
    {
        // IVPrismaUI1::Focus(PrismaView, bool pauseGame, bool disableFocusMenu)
        if (!g_api || view == kInvalidView) {
            logger::trace("PrismaUI[trace]: Focus rejected (api={} view=0x{:X})",
                          static_cast<const void*>(g_api),
                          static_cast<std::uint64_t>(view));
            return;
        }
        logger::trace("PrismaUI[trace]: Focus(handle=0x{:X} pause={} disableFocusMenu={})",
                      static_cast<std::uint64_t>(view),
                      pauseGame ? 1 : 0,
                      disableFocusMenu ? 1 : 0);
        g_api->Focus(view, pauseGame, disableFocusMenu);
    }

    void Unfocus(ViewHandle view)
    {
        // IVPrismaUI1::Unfocus(PrismaView)
        if (!g_api || view == kInvalidView) {
            logger::trace("PrismaUI[trace]: Unfocus rejected (api={} view=0x{:X})",
                          static_cast<const void*>(g_api),
                          static_cast<std::uint64_t>(view));
            return;
        }
        logger::trace("PrismaUI[trace]: Unfocus(handle=0x{:X})", static_cast<std::uint64_t>(view));
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

    bool HasAnyActiveFocus()
    {
        // IVPrismaUI1::HasAnyActiveFocus()
        if (!g_api) {
            return false;
        }
        return g_api->HasAnyActiveFocus();
    }

    void RegisterJSListener(ViewHandle view, const std::string& functionName, JSListenerCallback callback)
    {
        // IVPrismaUI1::RegisterJSListener(PrismaView, const char* functionName, JSListenerCallback)
        // Our wrapper's JSListenerCallback typedef is ABI-compatible with
        // PRISMA_UI_API::JSListenerCallback (same signature); reinterpret is
        // used to keep upstream headers out of PrismaUI.h.
        if (!g_api || view == kInvalidView || !callback) {
            logger::trace("PrismaUI[trace]: RegisterJSListener('{}') rejected "
                          "(api={} view=0x{:X} cb={})",
                          functionName,
                          static_cast<const void*>(g_api),
                          static_cast<std::uint64_t>(view),
                          callback ? "ok" : "null");
            return;
        }
        logger::trace("PrismaUI[trace]: RegisterJSListener('{}') on handle=0x{:X}",
                      functionName,
                      static_cast<std::uint64_t>(view));
        g_api->RegisterJSListener(
            view, functionName.c_str(), reinterpret_cast<PRISMA_UI_API::JSListenerCallback>(callback));
    }
} // namespace NarrativeEngine::PrismaUI_API
