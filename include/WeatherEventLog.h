#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>

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
// Threading: Poll() runs on the main thread from Tick, so engine reads
// (Sky, Calendar) are safe without marshaling. All state lives behind a
// single internal mutex so a future off-thread caller doesn't race with
// a mid-poll snapshot.
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

    // Main-thread poll driven by Tick's 500 ms loop. Internally
    // throttles to iWeatherEventPollIntervalSeconds of *unpaused*
    // elapsed time — the caller (Tick.cpp) only calls Poll when the
    // game is unpaused, and passes the wall-clock delta since the
    // previous PollOnMainThread cycle (~500ms during normal play).
    // WeatherEventLog accumulates those deltas and does the actual
    // sky sample once the accumulator crosses the interval. Weather
    // updates on the order of tens of seconds, so per-Tick sampling
    // would waste work without producing new signal.
    //
    // Suppressed when Sky::mode != kFull (interior — no visible sky).
    // Emits a weather_event on category-tuple change, subject to a
    // secondary inter-event debounce also measured in unpaused seconds.
    void Poll(double unpausedElapsedSeconds);

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
} // namespace NarrativeEngine::WeatherEventLog
