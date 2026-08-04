#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

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

    // What FormatEventsText does with a `book_read` event's `book_text`
    // field — the full body of the book the player just opened.
    //
    // Either way the markup is rendered to plain text (see BookTextRenderer);
    // the choice is only whether the body survives at all. It is genuinely
    // large: SkyrimNet hands us the whole book, and an illustrated one runs
    // to tens of kilobytes. A single such event is enough to dominate a
    // prompt, so every LLM-facing consumer wants `Omit` — none of them reason
    // about what a book *said*, only that one was read. `Render` exists for
    // the human-facing consumers (the dashboard timeline, the event history
    // log), where the body is the interesting part.
    //
    // Deliberately has no default: picking wrong is a silent prompt-size
    // blowup in one direction and silent data loss in the other, so each call
    // site states its choice.
    enum class BookTextPolicy
    {
        Render,
        Omit,
    };

    // Walks a parsed SkyrimNet events array in place. For each object event,
    // synthesizes an `evt.text` field based on `evt.type` and `evt.data`,
    // prepended with a relative-time bucket computed against the supplied
    // `currentGameTimeSeconds` (same units as SkyrimNet's per-event
    // `gameTime` field). Unknown types fall back to dumping `data` verbatim
    // so the consumer at least sees the raw content. Defensive against
    // missing fields and non-object entries.
    //
    // `book_read` events are additionally normalized in place: `data
    // .book_text` is rewritten to its rendered plain-text form, or erased
    // outright, per `bookText`. That mutation matters independently of the
    // synthesized `text` — the whole event object is serialized into the
    // prompt context, so leaving the raw markup on `data` would ship it
    // across the API boundary even though no template renders it.
    //
    // `playerName` names the reader in the rendered `book_read` line
    // ("Maxxor read \"The Lusty Argonian Maid\""). SkyrimNet's payload
    // carries no actor field — the event fires off the book menu, which only
    // the player opens — so the caller supplies the name rather than this
    // module reaching for `RE::PlayerCharacter` and dragging an engine
    // dependency into what is otherwise a pure JSON transform. Empty falls
    // back to "The player".
    void FormatEventsText(nlohmann::json& events,
                          double currentGameTimeSeconds,
                          BookTextPolicy bookText,
                          std::string_view playerName);

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
