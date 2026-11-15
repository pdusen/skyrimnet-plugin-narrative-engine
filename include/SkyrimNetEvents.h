#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

// Shared helpers for shaping SkyrimNet's per-event JSON into the timeline
// the Director's prompts and the dashboard both read. Each event gains a
// `line` record -- what happened, as fields, with a `kind` that says which --
// so no consumer has to branch on SkyrimNet's own `type` discriminator or
// read its `data`. EventText.h lists the records; the prompts and EventText
// word them.
namespace NarrativeEngine::SkyrimNetEvents
{
    // Social-media-style relative time bucketing. Precision tapers off the
    // further back in time we go: moments -> minutes -> hours -> days ->
    // weeks. Returns {"value": N, "unit": "minute"}, the `age` every timeline
    // entry and memory row carries; EventText.h lists the units. Words come
    // later, from whoever shows it: "5 minutes ago" before an event, "for 5
    // minutes" in the idle marker.
    nlohmann::json RelativeGameTime(double seconds);

    // In-world age of one memory row from GetMemoriesForActor, in game
    // seconds, or a negative value when the row carries no usable
    // `game_time`. `nowGameSeconds` is the caller's current game clock
    // (Calendar::GetHoursPassed() * 3600, or the GetDaysPassed() * 86400
    // equivalent — they share a base).
    //
    // USE THIS RATHER THAN THE ROW'S `age_hours`. That field is REAL-WORLD
    // elapsed time since the row was written, not game time: a save picked
    // up after a two-month break reports every memory in it as ~60 days
    // old while none of them has aged an in-world hour. Memory ages shown
    // to an LLM were wrong in exactly that way until this existed, and the
    // gossip harvester rejected every candidate it saw because of it.
    //
    // See docs/engine-findings/skyrimnet-memory-json-field-names.md.
    double MemoryAgeGameSeconds(const nlohmann::json& memoryRow, double nowGameSeconds);

    // What DescribeEvents does with a `book_read` event's `book_text`
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
    // adds a `line` record built from `evt.type` and `evt.data`, and, when the
    // event has a numeric `gameTime`, an `age` measured against the supplied
    // `currentGameTimeSeconds` (same units as SkyrimNet's per-event
    // `gameTime` field). Unknown types become a `text` line holding `data`
    // verbatim, so the consumer at least sees the raw content. Defensive
    // against missing fields and non-object entries.
    //
    // `book_read` events are additionally normalized in place: `data
    // .book_text` is rewritten to its rendered plain-text form, or erased
    // outright, per `bookText`. That mutation matters independently of the
    // `line` — the whole event object is serialized into the
    // prompt context, so leaving the raw markup on `data` would ship it
    // across the API boundary even though no template renders it.
    //
    // `playerName` is the `reader` of a `book_read` line. SkyrimNet's
    // payload carries no actor field -- the event fires off the book menu,
    // which only the player opens -- so the caller supplies the name rather
    // than this module reaching for `RE::PlayerCharacter` and dragging an
    // engine dependency into what is otherwise a pure JSON transform. Empty
    // is an unnamed player, and the wording says so.
    void DescribeEvents(nlohmann::json& events,
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
