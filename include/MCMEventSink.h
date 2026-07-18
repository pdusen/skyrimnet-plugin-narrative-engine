#pragma once

// C++ sink for the "_ne_DashboardHotkeyChanged" SKSE ModEvent that
// _ne_MCM.psc fires on any MCM setting change. MCM Helper writes the MCM
// override INI atomically before the event arrives, so the sink's job is
// simply to trigger a fresh Settings::ApplyMcmOverride read — no payload
// parsing needed.
//
// Marshals the reload onto the main thread; ModEvent sinks run on the
// Papyrus VM thread, and mutating Settings there is fine in practice but
// keeps this consistent with the rest of the codebase's discipline.

namespace NarrativeEngine::MCMEventSink
{
    // Register the ModEvent sink. Idempotent — safe to call more than
    // once. Should be invoked from Plugin.cpp's kDataLoaded handler
    // after Settings::Load().
    void Initialize();
} // namespace NarrativeEngine::MCMEventSink
