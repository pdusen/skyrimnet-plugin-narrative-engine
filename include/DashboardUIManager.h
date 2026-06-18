#pragma once

#include <string>

// In-game observability dashboard manager.
//
// Owns the PrismaUI browser view that hosts the React dashboard bundle
// (Step 15), composes the JSON state blob on every Director ApplyDecision
// (Step 14), pushes that blob into the view via PrismaUI's JS-interop
// fast path (PrismaUI_API::InvokeJS → window.updateFullState), and toggles
// view visibility on the configured hotkey.
//
// All of this degrades gracefully when PrismaUI is uninstalled — the
// Director loop runs unchanged; the dashboard manager simply no-ops.
namespace NarrativeEngine::DashboardUIManager
{
    // Create the PrismaUI view and register the hotkey input sink. Call once
    // from SKSE's kDataLoaded message, after PrismaUI_API::Initialize().
    // If PrismaUI is unavailable, logs and returns — every subsequent call
    // on this namespace silently no-ops.
    void Initialize();

    // Tear down the view. Not strictly required at process exit (Windows
    // cleans up) but declared for symmetry and clean restart.
    void Shutdown();

    // Compose the current Director state into the JSON shape declared in
    // `dashboard/src/types.ts` (`DirectorState`) and ship it to the view's
    // `window.updateFullState`. Called from EvaluationPipeline::ApplyDecision
    // on the main thread after every successful decision. No-op when
    // PrismaUI is unavailable.
    void PushFullState();

    // Show ↔ hide the dashboard view. On show, calls PushFullState first so
    // the view shows current state rather than a stale snapshot. Triggered
    // by the configured hotkey.
    void ToggleVisibility();

    // Pure function exposed for testing: compose the JSON state blob the
    // dashboard expects without actually pushing it to PrismaUI. Safe to
    // call from the main thread (it reads RE::Calendar for relative-time
    // computation in the events list).
    std::string ComposeFullStateJSON();
}
