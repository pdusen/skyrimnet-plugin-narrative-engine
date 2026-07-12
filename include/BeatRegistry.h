#pragma once

#include <IBeat.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// BeatRegistry — flat registry of IBeat instances.
//
// Beats register once at plugin startup (Plugin.cpp kDataLoaded, after
// BeatSystem::Initialize). Lookups happen on the main thread during beat
// selection. An internal mutex guards storage so the registry is safe to
// query from any thread; in practice registration is single-threaded at
// startup and reads are main-thread-only, so the mutex never sees
// contention.
//
// Per-beat runtime state carried alongside the IBeat pointer:
//   enabled: dispatched only when true. Seeded from Settings at Register
//            time (bEnableXxx keys) and toggleable at runtime via the
//            dashboard Dispatch tab. Runtime toggles do NOT write back to
//            Settings/INI.
//   lastDispatchedRealTime: Unix-epoch seconds of the most recent
//            successful StartBeat; zero until the first dispatch. Session-
//            only (not persisted) — for dashboard display.
namespace NarrativeEngine::BeatRegistry
{
    // No-op at kDataLoaded before any Register call. Logs the current
    // registration count so boot output is consistent across builds.
    void Initialize();

    // Take ownership of `beat` and add it to the registry. Duplicate
    // registrations (matching Name()) are ignored with a warning. Call
    // from the main thread at startup. The registry consults Settings to
    // seed the enabled flag from the matching bEnableXxx INI key.
    void Register(std::unique_ptr<IBeat> beat);

    // Snapshot view of a single registration. Value-copyable so the
    // dashboard payload builder can iterate without holding the registry
    // lock across engine calls.
    struct EntryView
    {
        IBeat* beat;
        std::string name;
        bool enabled;
        double lastDispatchedRealTime; // Unix-epoch seconds; 0 = never
    };

    // Snapshot of every registered beat + its runtime state, in
    // registration order. Called once per state push from the dashboard.
    std::vector<EntryView> All();

    // Lookup by name. Returns nullptr when unknown.
    IBeat* Find(std::string_view name);

    // Enable/disable a registered beat at runtime. Idempotent; silently
    // no-ops on unknown names. The dashboard Dispatch tab's per-row
    // checkbox flows through here.
    void SetEnabled(std::string_view name, bool enabled);
    bool IsEnabled(std::string_view name);

    // Stamp the most-recent-successful-dispatch timestamp on the named
    // beat. Called by BeatSystem::StartBeat when the top-level state
    // transitions to BEAT_RUNNING. Silently no-ops on unknown names.
    void MarkDispatched(std::string_view name);
    double LastDispatchedRealTime(std::string_view name);

    // All beats whose IsAvailable(ctx) returns true AND whose polarity is
    // compatible with the desired direction. Beats whose enabled flag is
    // false are skipped before IsAvailable is ever called. A beat with
    // polarity Either matches both Raise and Lower.
    std::vector<IBeat*> AvailableMatching(const BeatContext& ctx, BeatPolarity desired);
} // namespace NarrativeEngine::BeatRegistry
