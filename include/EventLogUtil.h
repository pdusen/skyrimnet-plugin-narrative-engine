#pragma once

#include <string>
#include <vector>

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

    // Render the current Skyrim calendar reading as a bracketed
    // absolute in-game timestamp: "[4E 201, Frostfall 15, 17:42:07]".
    // Used by the EventHistoryWriter to stamp each entry with an
    // absolute time rather than the "[N ago]" relative prefix used by
    // the LLM-facing tail. Returns "[unknown time]" if Calendar is
    // unavailable (should never fire outside of shutdown paths).
    std::string CurrentInGameTimestamp();

    // Decode a cumulative game-time-seconds value (as produced by
    // `NowGameTimeSeconds` and as carried on SkyrimNet events'
    // `gameTime` field) back into a Skyrim calendar date and format
    // it in the same "[4E 201, Frostfall 15, 17:42:07]" shape as
    // CurrentInGameTimestamp. Used by EventHistoryWriter to timestamp
    // SkyrimNet events with their emit-time date rather than the
    // flush-time date. Internal events don't need this — they capture
    // their timestamp string at emit via CurrentInGameTimestamp.
    //
    // Reference epoch: 17 Last Seed, 4E 201 (Skyrim's canonical game
    // start). Assumes a 365-day year, standard Skyrim month lengths.
    // Precision to the second.
    std::string FormatInGameTimestampFromGameTime(double gameTimeSeconds);

    // Shared row type consumed by EventHistoryWriter. Each internal
    // event log module (CombatEventLog / WeatherEventLog /
    // TravelEventLog) enqueues one HistoryEntry per emitted event
    // into a session-only pending queue; the writer drains those
    // queues plus a SkyrimNet event fetch on its own cadence, sorts
    // by localTime, and appends to the rotating history log file.
    struct HistoryEntry
    {
        double localTime = 0.0;      // Unix-epoch seconds; used for cross-source sort within a batch
        std::string inGameTimestamp; // "[4E 201, Frostfall 15, 17:42:07]" — captured at emit for internal
                                     // sources; synthesized at flush for SkyrimNet-sourced entries
        std::string sourceKind;      // "internal/weather_event/rain_start" or "skyrimnet/dialogue" etc.
        std::string body;            // rendered sentence body (no timestamp / sourceKind prefix)
    };

    // Convenience: return a HistoryEntry vector containing one entry
    // per event from the caller-supplied set. Callers each own their
    // pending queue's mutex.
    std::vector<HistoryEntry> DrainVector(std::vector<HistoryEntry>& pending);
} // namespace NarrativeEngine::EventLogUtil
