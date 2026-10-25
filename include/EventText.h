#pragma once

#include <LanguageTable.h>
#include <TextKeys.h>

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

// The words for one entry of the event timeline, for the readers that show it
// to a person: the dashboard's event list and the event history log.
//
// The timeline itself carries no sentences. Every entry the event sources
// emit (SkyrimNetEvents for SkyrimNet's own events, CombatEventLog,
// WeatherEventLog, TravelEventLog, and the merge's condensed and idle
// entries) holds a `line` record saying what happened, and, when it has a
// time, an `age` saying how long ago. The Director's prompts word the same
// records themselves, through components/narrative_engine/event_timeline
// .prompt, so the two wordings are kept in step by hand: a field added here
// is a field that prompt can use.
//
// `age` is a relative time already put into one bucket, the coarsest that
// still says something: {"value": 5, "unit": "minute"}. The units, and what
// `value` holds in each, are
//
//   moment   under a minute; value 0
//   minute   whole minutes, under an hour
//   hour     whole hours, under a day
//   day      whole days, under a week
//   week     whole weeks
//   unknown  no usable time; value 0 (sender dialogue only)
//
// `line` is an object whose `kind` says which fields it has. Every field
// listed is present, as a string unless noted, though it may be empty.
//
//   dialogue             speaker, listener, dialogue
//   player_dialogue      speaker, listener, dialogue
//   gamemaster_dialogue  speaker, target, topic, dialogue
//   npc_thoughts         name, thoughts
//   death                killer, victim
//   book_read            reader (empty for an unnamed player), title, body
//   text                 text -- a line another plugin wrote, used as it is
//   no_data              (none) -- an event that carried nothing to show
//
//   combat_start, combat_end, collapse, regain_footing
//                        actor
//   hit                  actor, target, source. `source` is actor,
//                        poison, spell_trap, trap, activator or unknown;
//                        `actor` names the attacker, or the activator, and
//                        is empty for the other sources
//   combat_summary       fights: [{aggressor, victim, shape}], others (bool),
//                        damage: [{target, source, actor, repeated (bool)}].
//                        `shape` is strike (one blow), attack (one side's
//                        blows) or trade (both sides'); `others` says more
//                        pairs fought than `fights` lists. A damage entry's
//                        source is actor only for a blow with no one named
//                        to receive it
//
//   weather              change: blizzard_start, storm_intensify,
//                        storm_subside, weather_shift, storm_start,
//                        rain_start, snow_start, rain_stop, snow_stop,
//                        clear_start or cloudy_start
//
//   travel               move, party (array of names; an empty name is an
//                        unnamed player), and by move:
//                          entered_location     place, interior (bool)
//                          left_location        place, interior (bool)
//                          crossed_holds        from, to
//                          entered_wilderness   origin, exited (bool), hold
//                          fast_travel_arrived  place, interior (bool),
//                                               origin (empty when unknown)
//                          journey              origin, arrival (either may
//                                               be empty)
//                          visit                places (array; may be empty)
//
//   idle                 duration: an `age`-shaped bucket, how long nothing
//                        has happened
//   idle_long            (none) -- quiet for longer than the timeline keeps
//
// The words are the [Event.*] sections of a language file: the dashboard
// renders with the player's language, the history log always with English.
namespace NarrativeEngine::EventText
{
    // Every key this module renders.
    [[nodiscard]] const std::vector<TextKeys::Key>& Keys();

    // "5 minutes ago", "just now".
    [[nodiscard]] std::string Ago(const LanguageTable::Table& table, const nlohmann::json& age);

    // "5 minutes", "less than a minute": an `age` read as a length of time.
    [[nodiscard]] std::string Duration(const LanguageTable::Table& table, const nlohmann::json& age);

    // What `line` says happened, with no time.
    [[nodiscard]] std::string Body(const LanguageTable::Table& table, const nlohmann::json& line);

    // One timeline entry as the dashboard shows it: "[5 minutes ago] Hans
    // strikes Luke", or the body alone for an entry with no `age`.
    [[nodiscard]] std::string Line(const LanguageTable::Table& table, const nlohmann::json& event);
} // namespace NarrativeEngine::EventText
