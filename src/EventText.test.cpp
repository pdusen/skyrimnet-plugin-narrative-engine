#include <EventText.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

// Characterization of the event timeline's wording: every record kind, every
// shape a kind's fields can take, and every age bucket, each beside the line
// the dashboard and the event history log show for it.
//
// Phase 18 Step 11 replaced the sentences the event sources composed with
// records, and moved their words into english.ini. Each expected line below
// is the one the replaced code wrote for the same event, but for one
// difference: a list of three or more names is joined as [Grammar] joins
// every list, with no comma before "and". The Director's prompts word the
// same records themselves, in components/narrative_engine/event_timeline
// .prompt, and keep that comma; a case's `prompt` is how the prompt reads it
// where that differs. The step's render comparison fed the prompts this
// table, so a case added here is a case that prompt should be checked against
// too.

namespace
{
    using nlohmann::json;

    // The shipped english.ini, read against EventText's keys alone: the
    // other sections' keys are rejected as unknown here, which matters to
    // nobody, because only these are rendered.
    const NarrativeEngine::LanguageTable::Table& English()
    {
        static const std::string text = [] {
            std::ifstream in(std::filesystem::path{NARRATIVEENGINE_STATICS_DIR}
                                 / "SKSE/Plugins/NarrativeEngine/Languages/english.ini",
                             std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }();
        static const auto table = NarrativeEngine::LanguageTable::Table::Build({text, std::nullopt, std::nullopt},
                                                                               NarrativeEngine::EventText::Keys());
        return table;
    }

    // [{"event": <timeline entry>, "line": <what it reads as>}, ...], in
    // pieces, since MSVC caps a single string literal at 16KB.
    constexpr const char* kCases =
        R"json([
{"event": {"type": "dialogue", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "dialogue", "speaker": "Ysolda", "listener": "Carlotta", "dialogue": "Good morning."}},
 "line": "[just now] Ysolda -> Carlotta: \"Good morning.\""},
{"event": {"type": "dialogue", "age": {"value": 1, "unit": "minute"},
           "line": {"kind": "dialogue", "speaker": "Ysolda", "listener": "", "dialogue": "Good morning."}},
 "line": "[1 minute ago] Ysolda: \"Good morning.\""},
{"event": {"type": "dialogue_player_text", "age": {"value": 5, "unit": "minute"},
           "line": {"kind": "player_dialogue", "speaker": "Maxxor", "listener": "Ysolda", "dialogue": "Hello."}},
 "line": "[5 minutes ago] (player) Maxxor -> Ysolda: \"Hello.\""},
{"event": {"type": "dialogue_player_text", "age": {"value": 1, "unit": "hour"},
           "line": {"kind": "player_dialogue", "speaker": "Maxxor", "listener": "", "dialogue": "Hello."}},
 "line": "[1 hour ago] (player) Maxxor: \"Hello.\""},
{"event": {"type": "gamemaster_dialogue", "age": {"value": 3, "unit": "hour"},
           "line": {"kind": "gamemaster_dialogue", "speaker": "Ysolda", "target": "Carlotta", "topic": "the market",
                    "dialogue": "Prices are up."}},
 "line": "[3 hours ago] Ysolda -> Carlotta (topic: the market): Prices are up."},
{"event": {"type": "npc_thoughts", "age": {"value": 1, "unit": "day"},
           "line": {"kind": "npc_thoughts", "name": "Ysolda", "thoughts": "I need more tusks."}},
 "line": "[1 day ago] Ysolda (thinking): \"I need more tusks.\""},
{"event": {"type": "death", "age": {"value": 2, "unit": "day"},
           "line": {"kind": "death", "killer": "Hans", "victim": "Luke"}},
 "line": "[2 days ago] Hans killed Luke"},
{"event": {"type": "book_read", "age": {"value": 1, "unit": "week"},
           "line": {"kind": "book_read", "reader": "Maxxor", "title": "The Lusty Argonian Maid", "body": ""}},
 "line": "[1 week ago] Maxxor read \"The Lusty Argonian Maid\""},
{"event": {"type": "book_read", "age": {"value": 3, "unit": "week"},
           "line": {"kind": "book_read", "reader": "", "title": "", "body": ""}},
 "line": "[3 weeks ago] The player read a book"},
{"event": {"type": "book_read",
           "line": {"kind": "book_read", "reader": "Maxxor", "title": "Notes", "body": "Page one.\nPage two."}},
 "line": "Maxxor read \"Notes\":\nPage one.\nPage two."},
{"event": {"type": "follower_left", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "text", "text": "Lydia stopped following you."}},
 "line": "[just now] Lydia stopped following you."},
{"event": {"type": "mystery", "age": {"value": 0, "unit": "moment"}, "line": {"kind": "no_data"}},
 "line": "[just now] (no data)"},

{"event": {"type": "combat_event", "ne_kind": "combat_start", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_start", "actor": "Maxxor"}},
 "line": "[just now] Maxxor enters combat"},
{"event": {"type": "combat_event", "ne_kind": "combat_end", "age": {"value": 2, "unit": "minute"},
           "line": {"kind": "combat_end", "actor": "Maxxor"}},
 "line": "[2 minutes ago] Maxxor leaves combat"},
{"event": {"type": "combat_event", "ne_kind": "collapse", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "collapse", "actor": "Luke"}},
 "line": "[just now] Luke collapses"},
{"event": {"type": "combat_event", "ne_kind": "regain_footing", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "regain_footing", "actor": "Luke"}},
 "line": "[just now] Luke regains their footing"},
{"event": {"type": "combat_event", "ne_kind": "hit", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "hit", "actor": "Hans", "target": "Luke", "source": "actor"}},
 "line": "[just now] Hans strikes Luke"},
{"event": {"type": "combat_event", "ne_kind": "hit", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "hit", "actor": "", "target": "Luke", "source": "poison"}},
 "line": "[just now] Luke took damage from poison"},
{"event": {"type": "combat_event", "ne_kind": "hit", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "hit", "actor": "", "target": "Luke", "source": "spell_trap"}},
 "line": "[just now] Luke took damage from a spell trap"},
{"event": {"type": "combat_event", "ne_kind": "hit", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "hit", "actor": "", "target": "Luke", "source": "trap"}},
 "line": "[just now] Luke took damage from a trap"},
{"event": {"type": "combat_event", "ne_kind": "hit", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "hit", "actor": "bear trap", "target": "Luke", "source": "activator"}},
 "line": "[just now] Luke took damage from bear trap"},
{"event": {"type": "combat_event", "ne_kind": "hit", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "hit", "actor": "", "target": "Luke", "source": "activator"}},
 "line": "[just now] Luke took damage"},
{"event": {"type": "combat_event", "ne_kind": "hit", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "hit", "actor": "", "target": "Luke", "source": "unknown"}},
 "line": "[just now] Luke took damage"},

{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary", "fights": [{"aggressor": "Hans", "victim": "Luke", "shape": "strike"}],
                    "others": false, "damage": []}},
 "line": "[just now] Hans strikes Luke"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary", "fights": [{"aggressor": "Hans", "victim": "Luke", "shape": "attack"}],
                    "others": false, "damage": []}},
 "line": "[just now] Hans attacks Luke"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary", "fights": [{"aggressor": "Hans", "victim": "Luke", "shape": "trade"}],
                    "others": false, "damage": []}},
 "line": "[just now] Hans and Luke trade blows"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 1, "unit": "minute"},
           "line": {"kind": "combat_summary",
                    "fights": [{"aggressor": "Alva", "victim": "Bran", "shape": "attack"},
                               {"aggressor": "Cira", "victim": "Dorn", "shape": "attack"}],
                    "others": false, "damage": []}},
 "line": "[1 minute ago] Combat continues: Alva attacks Bran; Cira attacks Dorn"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary",
                    "fights": [{"aggressor": "Alva", "victim": "Bran", "shape": "attack"},
                               {"aggressor": "Cira", "victim": "Dorn", "shape": "attack"},
                               {"aggressor": "Eir", "victim": "Fulk", "shape": "attack"}],
                    "others": true, "damage": []}},
 "line": "[just now] Combat continues: Alva attacks Bran; Cira attacks Dorn; Eir attacks Fulk, and others"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary", "fights": [], "others": false,
                    "damage": [{"target": "Hans", "source": "activator", "actor": "a bear trap", "repeated": false}]}},
 "line": "[just now] Hans took damage from a bear trap"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary", "fights": [], "others": false,
                    "damage": [{"target": "Hans", "source": "poison", "actor": "", "repeated": true},
                               {"target": "Luke", "source": "unknown", "actor": "", "repeated": true}]}},
 "line": "[just now] Hans took repeated damage from poison; Luke took repeated damage"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary", "fights": [{"aggressor": "Hans", "victim": "Luke", "shape": "strike"}],
                    "others": false,
                    "damage": [{"target": "Ysolda", "source": "trap", "actor": "", "repeated": false}]}},
 "line": "[just now] Hans strikes Luke; Ysolda took damage from a trap"},
{"event": {"type": "combat_event", "ne_kind": "combat_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "combat_summary", "fights": [], "others": false,
                    "damage": [{"target": "", "source": "actor", "actor": "Hans", "repeated": false}]}},
 "line": "[just now]  took damage from Hans"},

{"event": {"type": "weather_event", "ne_kind": "blizzard_start", "age": {"value": 4, "unit": "minute"},
           "line": {"kind": "weather", "change": "blizzard_start"}},
 "line": "[4 minutes ago] A blizzard has struck."},
{"event": {"type": "weather_event", "ne_kind": "storm_intensify", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "storm_intensify"}},
 "line": "[just now] The wind is picking up and the storm intensifies."},
{"event": {"type": "weather_event", "ne_kind": "storm_subside", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "storm_subside"}},
 "line": "[just now] The storm is subsiding."},
{"event": {"type": "weather_event", "ne_kind": "weather_shift", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "weather_shift"}},
 "line": "[just now] The weather has shifted."},
{"event": {"type": "weather_event", "ne_kind": "storm_start", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "storm_start"}},
 "line": "[just now] A thunderstorm is rolling in."},
{"event": {"type": "weather_event", "ne_kind": "rain_start", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "rain_start"}},
 "line": "[just now] Rain has started to fall."},
{"event": {"type": "weather_event", "ne_kind": "snow_start", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "snow_start"}},
 "line": "[just now] Snow is beginning to fall."},
{"event": {"type": "weather_event", "ne_kind": "rain_stop", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "rain_stop"}},
 "line": "[just now] The rain has stopped."},
{"event": {"type": "weather_event", "ne_kind": "snow_stop", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "snow_stop"}},
 "line": "[just now] The snow is letting up."},
{"event": {"type": "weather_event", "ne_kind": "clear_start", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "clear_start"}},
 "line": "[just now] The clouds have parted and the sun is coming out."},
{"event": {"type": "weather_event", "ne_kind": "cloudy_start", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "weather", "change": "cloudy_start"}},
 "line": "[just now] The sky has clouded over."},

)json"
        R"json({"event": {"type": "travel_event", "ne_kind": "entered_location", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "entered_location", "party": ["Maxxor"], "place": "The Bannered Mare",
                    "interior": true}},
 "line": "[just now] Maxxor entered The Bannered Mare."},
{"event": {"type": "travel_event", "ne_kind": "entered_location", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "entered_location", "party": ["Maxxor", "Lydia"], "place": "Whiterun",
                    "interior": false}},
 "line": "[just now] Maxxor and Lydia arrived at Whiterun."},
{"event": {"type": "travel_event", "ne_kind": "left_location", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "left_location", "party": [""], "place": "The Bannered Mare",
                    "interior": true}},
 "line": "[just now] The Player exited The Bannered Mare."},
{"event": {"type": "travel_event", "ne_kind": "left_location", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "left_location", "party": ["Maxxor"], "place": "Whiterun",
                    "interior": false}},
 "line": "[just now] Maxxor departed from Whiterun."},
{"event": {"type": "travel_event", "ne_kind": "crossed_holds", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "crossed_holds", "party": ["Maxxor", "Lydia", "Faendal"],
                    "from": "Whiterun Hold", "to": "Falkreath Hold"}},
 "line": "[just now] Maxxor, Lydia and Faendal crossed from Whiterun Hold into Falkreath Hold.",
 "prompt": "[just now] Maxxor, Lydia, and Faendal crossed from Whiterun Hold into Falkreath Hold."},
{"event": {"type": "travel_event", "ne_kind": "entered_wilderness", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "entered_wilderness", "party": ["Maxxor"], "origin": "Riverwood",
                    "exited": false, "hold": "Whiterun Hold"}},
 "line": "[just now] Maxxor departed from Riverwood and is now in the wilderness of Whiterun Hold."},
{"event": {"type": "travel_event", "ne_kind": "entered_wilderness", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "entered_wilderness", "party": ["Maxxor", "Lydia"],
                    "origin": "Bleak Falls Barrow", "exited": true, "hold": "Falkreath Hold"}},
 "line": "[just now] Maxxor and Lydia exited Bleak Falls Barrow and are now in the wilderness of Falkreath Hold."},
{"event": {"type": "travel_event", "ne_kind": "fast_travel_arrived", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "fast_travel_arrived", "party": ["Maxxor"], "place": "Riverwood",
                    "interior": false, "origin": "Whiterun"}},
 "line": "[just now] Maxxor arrived at Riverwood, having journeyed from Whiterun."},
{"event": {"type": "travel_event", "ne_kind": "fast_travel_arrived", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "fast_travel_arrived", "party": ["Maxxor"], "place": "Sleeping Giant Inn",
                    "interior": true, "origin": ""}},
 "line": "[just now] Maxxor entered Sleeping Giant Inn."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "journey", "party": ["Maxxor"], "origin": "Whiterun",
                    "arrival": "Riverwood"}},
 "line": "[just now] Maxxor travelled from Whiterun to Riverwood."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "journey", "party": ["Maxxor"], "origin": "", "arrival": "Riverwood"}},
 "line": "[just now] Maxxor ended up in Riverwood."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "journey", "party": ["Maxxor"], "origin": "Whiterun", "arrival": ""}},
 "line": "[just now] Maxxor set out from Whiterun."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "journey", "party": ["Maxxor"], "origin": "", "arrival": ""}},
 "line": "[just now] Maxxor travelled a while."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "visit", "party": ["Maxxor"], "places": ["The Bannered Mare"]}},
 "line": "[just now] Maxxor visited The Bannered Mare."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "visit", "party": ["Maxxor"],
                    "places": ["The Bannered Mare", "Belethor's General Goods"]}},
 "line": "[just now] Maxxor visited The Bannered Mare and Belethor's General Goods."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "visit", "party": ["Maxxor"],
                    "places": ["The Bannered Mare", "Belethor's General Goods", "Dragonsreach"]}},
 "line": "[just now] Maxxor visited The Bannered Mare, Belethor's General Goods and Dragonsreach.",
 "prompt": "[just now] Maxxor visited The Bannered Mare, Belethor's General Goods, and Dragonsreach."},
{"event": {"type": "travel_event", "ne_kind": "travel_summary", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "visit", "party": ["Maxxor"], "places": []}},
 "line": "[just now] Maxxor looked around."},
{"event": {"type": "travel_event", "ne_kind": "unknown", "age": {"value": 0, "unit": "moment"},
           "line": {"kind": "travel", "move": "unknown", "party": ["Maxxor"]}},
 "line": "[just now] Maxxor travelled somewhere."},

{"event": {"type": "idle_marker", "ne_kind": "idle_marker",
           "line": {"kind": "idle", "duration": {"value": 11, "unit": "minute"}}},
 "line": "Nothing notable has happened for 11 minutes"},
{"event": {"type": "idle_marker", "ne_kind": "idle_marker",
           "line": {"kind": "idle", "duration": {"value": 1, "unit": "hour"}}},
 "line": "Nothing notable has happened for 1 hour"},
{"event": {"type": "idle_marker", "ne_kind": "idle_marker",
           "line": {"kind": "idle", "duration": {"value": 0, "unit": "moment"}}},
 "line": "Nothing notable has happened for less than a minute"},
{"event": {"type": "idle_marker", "ne_kind": "idle_marker", "line": {"kind": "idle_long"}},
 "line": "Nothing notable has happened for over an hour"}
])json";
} // namespace

TEST_CASE("EventText: every timeline record reads as it did", "[EventText]")
{
    const json cases = json::parse(kCases);
    for (const auto& c : cases) {
        INFO(c.at("event").dump());
        CHECK(NarrativeEngine::EventText::Line(English(), c.at("event")) == c.at("line").get<std::string>());
    }
}

TEST_CASE("EventText: an age", "[EventText]")
{
    using NarrativeEngine::EventText::Ago;
    using NarrativeEngine::EventText::Duration;

    SECTION("when it is unknown")
    {
        SECTION("should read as recent")
        {
            CHECK(Ago(English(), json{{"value", 0}, {"unit", "unknown"}}) == "recent");
        }
    }

    SECTION("when it is read as a length of time")
    {
        SECTION("should drop the ago")
        {
            CHECK(Duration(English(), json{{"value", 1}, {"unit", "minute"}}) == "1 minute");
            CHECK(Duration(English(), json{{"value", 3}, {"unit", "week"}}) == "3 weeks");
        }
    }
}
