#pragma once

#include <string>

namespace SKSE
{
    class SerializationInterface;
}

// Small helpers shared across the internal event-log modules
// (CombatEventLog, WeatherEventLog, TravelEventLog). Extracted from
// CombatEventLog once a second consumer arrived — no behavior change from
// their prior private CombatEventLog definitions.
//
// Threading: NowUnixSeconds / NowGameTimeOfDaySeconds are safe to call
// from any thread (chrono / RE::Calendar singletons are stable). The
// SerializationInterface helpers must only be called from SKSE's save /
// load callback threads, matching how CombatEventLog uses them today.
namespace NarrativeEngine::EventLogUtil
{
    // Unix-epoch seconds. Used as the wall-clock timestamp on every
    // internal event so age comparisons (phase-prune, condensation
    // windows, debounce gates) work consistently across the log modules.
    double NowUnixSeconds();

    // Cumulative game-time seconds since the Skyrim calendar epoch —
    // `Calendar::GetDaysPassed() * 86400`, which already includes the
    // intra-day fraction. Matches the units SkyrimNet uses for per-event
    // `gameTime` and the units the Snapshot builds `player.gameTimeSeconds`
    // in, so the merged `recent_events` array's "[N ago]" prefixes and the
    // downstream age-filter comparisons work uniformly across internal
    // and SkyrimNet-sourced entries.
    //
    // Historical note: an earlier revision of this helper returned
    // time-of-day only (`GetHour() * 3600`, [0..86400)). That silently
    // broke every internal event on any save past day 1 — the age filter
    // in BuildMergedTimeline compares to cumulative game-time and would
    // find every event to be "millions of seconds old." Fixed to
    // cumulative.
    double NowGameTimeSeconds();

    // Length-prefixed string write. Layout: uint32 length, then raw bytes.
    // Zero-length strings write only the length prefix.
    void WriteString(SKSE::SerializationInterface* intfc, const std::string& s);

    // Inverse of WriteString. Returns false on any short read; leaves
    // `out` in a partial state on failure. Callers should treat a false
    // return as "abort record parse and revert."
    bool ReadString(SKSE::SerializationInterface* intfc, std::string& out);
} // namespace NarrativeEngine::EventLogUtil
