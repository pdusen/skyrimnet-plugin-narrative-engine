#pragma once

#include <IAction.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ActionRegistry — flat registry of IAction instances.
//
// Actions register once at plugin startup (Plugin.cpp kDataLoaded, after
// the ActionDispatcher initializes). Lookups happen on the main thread
// during action selection. An internal mutex guards the storage so the
// registry is safe to query from any thread; in practice registration
// is single-threaded at startup and reads are main-thread-only, so the
// mutex never sees contention.
//
// Per-action runtime state carried alongside the IAction pointer:
//   enabled: dispatched only when true. Seeded from Settings at Register
//            time and toggleable at runtime via the dashboard Dispatch
//            tab. Runtime toggles do NOT write back to Settings/INI.
//   lastDispatchedRealTime: Unix-epoch seconds of the most recent
//            successful Start; zero until the first successful dispatch.
//            Session-only (not persisted) — for dashboard display.
namespace NarrativeEngine::ActionRegistry
{
    // Take ownership of `action` and add it to the registry. Duplicate
    // registrations (matching Name()) are ignored with a warning, so calling
    // Register twice for the same action is harmless. Call from the main
    // thread at startup. The registry consults Settings to seed the
    // enabled flag from the matching bEnableXxx INI key.
    void Register(std::unique_ptr<IAction> action);

    // Snapshot view of a single registration. Value-copyable so the
    // dashboard payload builder can iterate without holding the
    // registry lock across engine calls.
    struct EntryView
    {
        IAction*    action;
        std::string name;
        bool        enabled;
        double      lastDispatchedRealTime;  // Unix-epoch seconds; 0 = never
    };

    // Snapshot of every registered action + its runtime state, in
    // registration order. Called once per state push from the
    // dashboard — cheap (2-3 entries) and gives the caller a stable
    // view that won't shift under it.
    std::vector<EntryView> All();

    // Lookup by name. Returns nullptr when unknown.
    IAction* Find(std::string_view name);

    // Enable/disable a registered action at runtime. Idempotent;
    // silently no-ops on unknown names. The dashboard Dispatch tab's
    // per-row checkbox flows through here.
    void SetEnabled(std::string_view name, bool enabled);
    bool IsEnabled(std::string_view name);

    // Stamp the most-recent-successful-dispatch timestamp on the
    // named action. Called by ActionDispatcher when an action's
    // Start() reports started=true (both the normal path and the
    // force-dispatch path). Silently no-ops on unknown names.
    void MarkDispatched(std::string_view name);
    double LastDispatchedRealTime(std::string_view name);

    // All actions whose IsAvailable(ctx) returns true AND whose polarity
    // is compatible with the desired direction. Actions whose enabled
    // flag is false are skipped before IsAvailable is ever called. An
    // action with polarity Either matches both Raise and Lower.
    std::vector<IAction*> AvailableMatching(const ActionContext& ctx,
                                            ActionPolarity        desired);
}
