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

    // Walks a parsed SkyrimNet events array in place. For each object event,
    // synthesizes an `evt.text` field based on `evt.type` and `evt.data`,
    // prepended with a relative-time bucket computed against the supplied
    // `currentGameTimeSeconds` (same units as SkyrimNet's per-event
    // `gameTime` field). Unknown types fall back to dumping `data` verbatim
    // so the consumer at least sees the raw content. Defensive against
    // missing fields and non-object entries.
    void FormatEventsText(nlohmann::json& events, double currentGameTimeSeconds);

    // Merges a SkyrimNet event array (already FormatEventsText'd, oldest-
    // first) with a CombatEventLog rendered tail (oldest-first), produces a
    // single oldest-first timeline sorted ascending by `localTime`, and
    // condenses runs of consecutive internal `hit` events between non-hit
    // events into one summary entry. Discriminates internal events by the
    // presence of `ne_kind` — SkyrimNet events have no such field. The
    // condensed entry carries `type = "narrative_engine_combat_summary"` so
    // the dashboard can style it if it wants; the LLM reads only `text`.
    nlohmann::json BuildMergedTimeline(nlohmann::json skyrimNetEvents,
                                       nlohmann::json combatEvents,
                                       double         currentGameTimeSeconds);
}
