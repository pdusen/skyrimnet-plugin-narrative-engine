#pragma once

#include <cstdint>
#include <string>

// Typed, defensive wrapper over PrismaUI's runtime-loaded modder API.
//
// PrismaUI ships as an SKSE plugin (`PrismaUI.dll`) and exposes a single
// `RequestPluginAPI` export that hands back a versioned virtual interface
// (`IVPrismaUI1`/`IVPrismaUI2`). The upstream header lives outside our source
// tree at `$PRISMA_UI_INCLUDE/PrismaUI_API.h` and is included by exactly one
// translation unit: `src/PrismaUI.cpp`. (Our wrapper is intentionally named
// `PrismaUI.h` rather than `PrismaUI_API.h` so it doesn't shadow the
// upstream header on the include path — they'd otherwise share a filename.)
//
// PrismaUI is a runtime soft dependency: when it's absent every wrapper here
// no-ops gracefully so the rest of the plugin keeps running.
namespace NarrativeEngine::PrismaUI_API
{
    // Opaque handle to a PrismaUI browser view. Mirrors `PrismaView`
    // (`uint64_t`) from the upstream header so callers don't need to include
    // it transitively.
    using ViewHandle = std::uint64_t;
    inline constexpr ViewHandle kInvalidView = 0;

    // Resolves PrismaUI.dll and caches the V1 interface pointer. Call once
    // from SKSE's kDataLoaded message. Idempotent. Returns true iff PrismaUI
    // was located and its V1 interface was handed back.
    bool Initialize();

    // True after a successful Initialize(); false if PrismaUI is uninstalled
    // or Initialize hasn't run yet.
    bool IsAvailable();

    // Create a browser view from a local HTML file. Returns `kInvalidView` if
    // PrismaUI is unavailable or view creation fails.
    ViewHandle CreateView(const std::string& htmlPath);

    // Tear down a previously-created view. Safe to call on `kInvalidView` or
    // when PrismaUI is unavailable.
    void Destroy(ViewHandle view);

    // True if the view handle still refers to a live PrismaUI view.
    bool IsValid(ViewHandle view);

    // Show / hide. No-op when PrismaUI is unavailable or the handle is invalid.
    void Show(ViewHandle view);
    void Hide(ViewHandle view);

    // Focus / unfocus. Calling Show alone isn't enough to actually render
    // the overlay above the game on most setups — the view also needs
    // input focus. `pauseGame` pauses the underlying game while focused;
    // `disableFocusMenu` suppresses PrismaUI's "press X to release focus"
    // hint. No-op when PrismaUI is unavailable or the handle is invalid.
    void Focus(ViewHandle view, bool pauseGame, bool disableFocusMenu);
    void Unfocus(ViewHandle view);

    // True if the view is currently hidden (i.e. created but not visible).
    // Returns true when PrismaUI is unavailable — "hidden" is the sensible
    // default for a view that doesn't exist.
    bool IsHidden(ViewHandle view);

    // Calls `window.<functionName>(argument)` in the view's JavaScript
    // context using PrismaUI's JS-interop fast path. The argument is a single
    // string — for structured data, serialize it as JSON before calling.
    void InvokeJS(ViewHandle view, const std::string& functionName, const std::string& argument);

    // True if any PrismaUI view — from any mod, not just ours — currently
    // holds focus. Used to suppress our own hotkey when another mod's
    // PrismaUI overlay is up, so we don't stack overlays. Returns false
    // when PrismaUI is unavailable.
    bool HasAnyActiveFocus();

    // Signature for a JS -> C++ listener. `argument` is the single string
    // the JS side passes to `window.<functionName>(arg)`. The callback is
    // invoked on PrismaUI's worker thread — marshal to the main thread
    // before touching engine state.
    using JSListenerCallback = void (*)(const char* argument);

    // Registers a JS listener so calls to `window.<functionName>(arg)` in
    // the view's JavaScript context route into `callback`. No-op when
    // PrismaUI is unavailable or the handle is invalid.
    void RegisterJSListener(ViewHandle view, const std::string& functionName, JSListenerCallback callback);
} // namespace NarrativeEngine::PrismaUI_API
