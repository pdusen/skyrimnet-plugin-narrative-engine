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

// NarrativeEngine-owned weather event source. Fills the gap SkyrimNet's
// event log leaves around ambient weather — sun breaking through after
// a storm, snow starting to fall, a thunderstorm rolling in. Polls
// `RE::Sky` state on a throttled cadence, derives a small category tuple
// from the current weather's authored flags + lightning frequency, and
// emits a `weather_event` when the tuple changes. Rendered entries merge
// into the same `recent_events` array `CombatEventLog` already
// contributes to.
//
// Deliberately does NOT suppress override weathers (from other mods,
// console `sw`/`fw`, or a future portent beat) — those changes ARE
// narratively meaningful and belong in the tail. The observation loop is
// source-agnostic; the beat causes the change and the poll observes it,
// no side channel required. Scripted-cinematic weathers (Helgen attack,
// MQ206, DA02) are similarly left in the observation set — a story cue
// like the Helgen sky IS an event we want the Director to see.
//
// Threading: Poll() runs on the plugin thread from Tick and reaches
// into main via MainThread::Run to fetch the sky snapshot. All internal
// state lives behind a single mutex so the dashboard's main-thread
// GetRenderedTail reads don't race with plugin-thread mutations.
namespace NarrativeEngine::WeatherEventLog
{
    // SKSE co-save record type ID. Frozen — changing it orphans every
    // previously-saved WeatherEventLog payload.
    inline constexpr std::uint32_t kRecordTypeId = 'NEWE';

    // Called at kDataLoaded. No sinks to register — the observation is
    // pure poll. Reserved for future symmetry with CombatEventLog::Initialize.
    void Initialize();

    // Called at kPostLoadGame. Seeds `g_lastCategory` from the current
    // sky state and stamps `g_lastPolledAt` / `g_lastEmittedAt` so no
    // event fires on the first post-load Poll.
    void OnPostLoadGame();

    // Called by PhaseTracker after AdvanceTo / Reset commits the new
    // phase. Drops events older than PhaseTracker::PhaseEnteredAtRealTime()
    // so weather from the prior phase doesn't leak into the new phase's
    // narrative context.
    void OnPhaseAdvanced();

    // Plugin-thread poll driven by Tick's 500 ms loop. Internally
    // throttles to iWeatherEventPollIntervalSeconds of *unpaused*
    // elapsed time — the caller (Tick.cpp) only calls Poll when the
    // game is unpaused, and passes the wall-clock delta since the
    // previous poll cycle (~500ms during normal play). WeatherEventLog
    // accumulates those deltas and does the actual sky sample once the
    // accumulator crosses the interval. Weather updates on the order
    // of tens of seconds, so per-Tick sampling would waste work
    // without producing new signal.
    //
    // Sky sample runs on main via MainThread::Run + MainThreadEngine::
    // ReadCurrentSky; the category derivation, diff, debounce, and
    // ring-buffer mutation all run on the plugin thread against
    // mutex-guarded state.
    //
    // Suppressed when Sky::mode != Full (interior — no visible sky).
    // Emits a weather_event on category-tuple change, subject to a
    // secondary inter-event debounce also measured in unpaused seconds.
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);

    // Returns a JSON array of currently-retained events shaped like
    // SkyrimNet events:
    //   { type, ne_kind, localTime, gameTime, originatingActorName,
    //     targetActorName, text }
    // Oldest-first. `text` is pre-rendered with the same "[N ago]"
    // relative-game-time prefix FormatEventsText produces so the merger
    // can interleave with SkyrimNet events transparently.
    //
    // Step 3 ships this as an empty-array stub; Step 4 fills in the
    // rendering vocabulary.
    nlohmann::json GetRenderedTail(double currentGameTimeSeconds);

    // SKSE co-save persistence. OnSave prunes-then-writes so on-disk
    // content matches the in-memory rules — no resurrection of purged
    // events across a save/load.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();

    // Drain the session-only pending history queue for the
    // EventHistoryWriter. Called on the writer's flush cadence.
    // Each entry corresponds to one emitted weather_event and carries
    // its absolute in-game timestamp (captured at emit time) plus the
    // rendered sentence body. Not persisted; anything pending at
    // save/load boundary is written to the outgoing session's history
    // file and discarded.
    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail();
} // namespace NarrativeEngine::WeatherEventLog
