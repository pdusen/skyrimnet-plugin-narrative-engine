#pragma once

#include <IAction.h>

#include <memory>
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
namespace NarrativeEngine::ActionRegistry
{
    // Take ownership of `action` and add it to the registry. Duplicate
    // registrations (matching Name()) are ignored with a warning, so calling
    // Register twice for the same action is harmless. Call from the main
    // thread at startup.
    void Register(std::unique_ptr<IAction> action);

    // All registered actions, in registration order. Stable for the
    // plugin's lifetime; do not retain pointers past plugin shutdown.
    const std::vector<std::unique_ptr<IAction>>& All();

    // Lookup by name. Returns nullptr when unknown.
    IAction* Find(std::string_view name);

    // All actions whose IsAvailable(ctx) returns true AND whose polarity
    // is compatible with the desired direction. An action with polarity
    // Either matches both Raise and Lower.
    std::vector<IAction*> AvailableMatching(const ActionContext& ctx,
                                            ActionPolarity        desired);
}
