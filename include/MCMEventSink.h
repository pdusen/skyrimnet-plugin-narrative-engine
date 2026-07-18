#pragma once

// C++ sink for the "_ne_DashboardHotkeyChanged" SKSE ModEvent that
// _ne_MCM.psc fires when the player rebinds the dashboard hotkey and again
// on save-load via SKI_QuestBase::OnGameReload. Each event carries the
// (DXSC, modifier-bitmask) tuple; the sink updates Settings' two hotkey
// fields in place so HotkeySink picks up the new binding immediately.
//
// Marshals the update onto the main thread — ModEvent sinks run on the
// Papyrus VM thread and touching Settings there is fine in practice, but
// keeping the mutation on main matches the rest of the codebase's
// discipline.

namespace NarrativeEngine::MCMEventSink
{
    // Register the ModEvent sink. Idempotent — safe to call more than
    // once. Should be invoked from Plugin.cpp's kDataLoaded handler
    // after Settings::Load() so the first ModEvent (fired by
    // OnGameReload on save-load) has a populated Config to overwrite.
    void Initialize();
} // namespace NarrativeEngine::MCMEventSink
