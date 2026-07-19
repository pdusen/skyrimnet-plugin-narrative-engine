#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

// Shared helpers for rendering SkyrimNet's per-event JSON into a form the
// LLM prompt and the dashboard both consume. The C++-side synthesizes a
// human-readable `text` field on each event so downstream consumers don't
// have to branch on the SkyrimNet `type` discriminator themselves.
namespace NarrativeEngine::SkyrimNetEvents
{
    // Social-media-style relative time bucketing. Precision tapers off the
    // further back in time we go: seconds → minutes → hours → days → weeks.
    // Used as the leading "[N units ago]" prefix on each rendered event line.
    std::string FormatRelativeGameTime(double secondsAgo);

    // Bare-duration variant of FormatRelativeGameTime — drops the trailing
    // "ago" so it can be substituted into "for X" / "in X" phrasing.
    // ("5 minutes" instead of "5 minutes ago", "2 hours" instead of "2 hours
    // ago"). Sub-minute durations render as "less than a minute" so the
    // phrasing stays grammatical.
    std::string FormatRelativeGameDuration(double seconds);

    // Walks a parsed SkyrimNet events array in place. For each object event,
    // synthesizes an `evt.text` field based on `evt.type` and `evt.data`,
    // prepended with a relative-time bucket computed against the supplied
    // `currentGameTimeSeconds` (same units as SkyrimNet's per-event
    // `gameTime` field). Unknown types fall back to dumping `data` verbatim
    // so the consumer at least sees the raw content. Defensive against
    // missing fields and non-object entries.
    void FormatEventsText(nlohmann::json& events, double currentGameTimeSeconds);

    // Merges the SkyrimNet event tail with each internal-source tail
    // (combat + weather + travel), sorts the union ascending by
    // `localTime`, and condenses runs of consecutive internal `hit`
    // events between non-hit events into one summary entry. Weather and
    // travel events are surfaced as-is — travel's own GetRenderedTail
    // does per-source condensation before the merge, and weather events
    // are discrete narrative moments. Discriminates internal events by
    // the presence of `ne_kind` — SkyrimNet events have no such field.
    nlohmann::json BuildMergedTimeline(nlohmann::json skyrimNetEvents,
                                       nlohmann::json combatEvents,
                                       nlohmann::json weatherEvents,
                                       nlohmann::json travelEvents,
                                       double currentGameTimeSeconds);
} // namespace NarrativeEngine::SkyrimNetEvents
