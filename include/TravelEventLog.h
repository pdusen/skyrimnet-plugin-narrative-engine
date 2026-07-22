#pragma once

#include <EventLogUtil.h>
#include <PluginThread.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <vector>

namespace SKSE
{
    class SerializationInterface;
}

// NarrativeEngine-owned travel event source. Fills the gap SkyrimNet's
// event log leaves around the player's geographic trajectory — entering
// / leaving named locations, crossing hold boundaries, arriving via
// fast travel. Poll-based; runs every unpaused Tick cycle so cell-load
// transient transitions aren't missed.
//
// Emitted event kinds (all shape `type: "travel_event"`, discriminated
// by `ne_kind`):
//   - entered_location  — a named BGSLocation was entered
//   - left_location     — a named BGSLocation was left
//   - crossed_holds     — hold-region FormID changed (exterior<->exterior)
//   - entered_wilderness — location went null while hold stayed the same
//   - fast_travel_arrived — the next transition after a
//                           TESFastTravelEndEvent (Step 7)
//
// Interior gate: interior-to-interior transitions (loading-door hops
// between buildings, dungeon-zone changes) are suppressed. Interior <->
// exterior transitions still fire (entering / leaving a building);
// hold-region tracking is preserved across the interior visit so the
// re-emergence poll can still detect hold changes.
//
// Threading: Poll runs on the plugin thread from Tick and reaches into
// main via MainThread::Run to fetch the location/hold snapshot. All
// state lives behind an internal mutex so the dashboard's main-thread
// GetRenderedTail reads don't race with plugin-thread mutations. Event
// sinks (TESFastTravelEndEvent) run on foreign engine threads and
// mutate their own mutex-guarded state independently of the poll body.
namespace NarrativeEngine::TravelEventLog
{
    // SKSE co-save record type ID. Frozen — changing it orphans every
    // previously-saved TravelEventLog payload.
    inline constexpr std::uint32_t kRecordTypeId = 'NETR';

    // Called at kDataLoaded. Registers the TESFastTravelEndEvent sink
    // (Step 7).
    void Initialize();

    // Best-effort sink deregistration.
    void Shutdown();

    // Called at kPostLoadGame. Seeds g_lastSnapshot from the current
    // world state and marks the baseline initialized so the first Poll
    // doesn't emit a bogus initial transition.
    void OnPostLoadGame();

    // Called by PhaseTracker after AdvanceTo / Reset commits the new
    // phase. Drops events older than PhaseTracker::PhaseEnteredAtRealTime()
    // so travel from the prior phase doesn't leak into the new phase's
    // narrative context.
    void OnPhaseAdvanced();

    // Plugin-thread poll driven by Tick's 500 ms loop when the game
    // is unpaused. No throttle — cell-load transitions can be
    // transient and we want them all. `unpausedElapsedSeconds` is
    // accepted for symmetry with the Tick-driven-accumulator pattern
    // (see feedback_tick_driven_accumulators memory); currently
    // unused because Travel has no cadenced work, but the signature
    // keeps the door open for future timing gates.
    //
    // The location/hold snapshot fetch runs on main via
    // MainThread::Run; the diff, party collection dispatch, and
    // ring-buffer mutation run on the plugin thread against
    // mutex-guarded state.
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);

    // JSON array of currently-retained events, oldest-first. Step 6
    // ships this as an empty-array stub; Step 8 fills in the rendering
    // vocabulary and the run-collapsing condensation pass.
    nlohmann::json GetRenderedTail(double currentGameTimeSeconds);

    // SKSE co-save persistence. OnSave prunes-then-writes so on-disk
    // content matches the in-memory pruning rules.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();

    // Drain the session-only pending history queue for the
    // EventHistoryWriter. See WeatherEventLog::DrainHistoryTail for
    // shape / semantics — identical contract.
    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail();
} // namespace NarrativeEngine::TravelEventLog
