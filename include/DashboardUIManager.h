#pragma once

#include <string>
#include <unordered_map>

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
    // Values that ComposeFullStateJSON needs but that must be read on
    // the main thread (RE::Calendar and IBeat::RemainingCooldownGameHours
    // per the IBeat contract). PushFullState bundles both into a single
    // MainThread::Run so the JSON compose itself can run entirely on
    // the plugin thread with just this pre-gathered snapshot.
    struct DashboardEngineReads
    {
        // Player's absolute game-time in seconds (GetDaysPassed * 86400).
        // Used for the `recent_events` merged-timeline "relative-time"
        // rendering.
        double currentGameTimeSeconds = 0.0;
        // Per-beat remaining-cooldown-hours keyed by beat name. Absent
        // entries fall back to 0.0 in the JSON.
        std::unordered_map<std::string, double> beatCooldownHours;
    };

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
    // `window.updateFullState`.
    //
    // Safe from any thread. Internally: early-outs if the dashboard is
    // hidden (audit-fix finding 4 — no compose while nobody's looking),
    // then enqueues the actual work onto the plugin thread via
    // AsyncDispatch. The single-hop MainThread::Run inside covers
    // Calendar + IBeat::RemainingCooldownGameHours; the rest of the
    // compose runs off main.
    //
    // The early-out check + the enqueue are the only synchronous work
    // performed at the caller's thread; both are cheap.
    void PushFullState();

    // Show ↔ hide the dashboard view. On show, calls PushFullState first so
    // the view shows current state rather than a stale snapshot. Triggered
    // by the configured hotkey.
    void ToggleVisibility();

    // Pure function exposed for testing: compose the JSON state blob the
    // dashboard expects without actually pushing it to PrismaUI. The
    // caller pre-fills `reads` with values that must be read on the
    // main thread; the compose itself is fully thread-safe against the
    // rest of the plugin (LetterPool / BeatRegistry / PhaseTracker /
    // DecisionLog / VisitState / VisitConclusionPoll are all
    // mutex-guarded readers).
    std::string ComposeFullStateJSON(const DashboardEngineReads& reads);
} // namespace NarrativeEngine::DashboardUIManager
