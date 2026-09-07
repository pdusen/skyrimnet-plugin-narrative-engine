#include <SkyrimNetEvents.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// Unit tests for the SkyrimNet event rendering and timeline merge.
//
// Pure JSON-in, JSON-out, with no engine anywhere: the module deliberately
// takes the player's name and the current game clock as arguments rather than
// reaching for RE::PlayerCharacter or the Calendar, precisely so the transform
// stays independent of the game. That decision is what makes this testable, and
// the header says so.
//
// What is worth testing here is not the string formatting for its own sake but
// what the strings are used for: everything below ends up inside a Director
// prompt. An event rendered wrong is a lie told to the LLM, an event rendered
// too big blows the request, and a stale event the merge failed to drop keeps
// tension pinned high after a fight has ended. Each of those has happened.
//
// Malformed field handling is a first-class concern here, not an edge case.
// SkyrimNet emits explicit nulls routinely, and reading a field with nlohmann's
// own `value(key, def)` throws type_error.302 on a present-but-null key rather
// than falling back. That defect lived in this module until these tests found
// it: one null field threw out of the whole batch, taking every other event's
// rendering with it. The cases below pin the fixed behaviour.

namespace
{
    using nlohmann::json;
    namespace Events = NarrativeEngine::SkyrimNetEvents;
    using Events::BookTextPolicy;

    // The bucket boundaries the formatters step across, named rather than
    // spelled as literals so a case reads as "one second short of a minute"
    // instead of 59.
    constexpr double kMinute = 60.0;
    constexpr double kHour = 3600.0;
    constexpr double kDay = 86400.0;
    constexpr double kWeek = 7.0 * 86400.0;

    // A game clock far enough from zero that an event placed before it is
    // still positive. Zero would collide with the "no usable time" sentinel.
    constexpr double kNow = 100000.0;

    // FormatEventsText works on an array in place, so every case would
    // otherwise repeat the same wrap-index-read dance. These do it once.
    json FormatOne(json event, double now, BookTextPolicy policy, std::string_view player)
    {
        json events = json::array();
        events.push_back(std::move(event));
        Events::FormatEventsText(events, now, policy, player);
        return events[0];
    }

    std::string RenderOne(json event, BookTextPolicy policy = BookTextPolicy::Omit)
    {
        // The events built below carry no gameTime, so no relative-time prefix
        // appears in cases that are about the body of the line. The prefix has
        // its own test case, which supplies one deliberately.
        return FormatOne(std::move(event), kNow, policy, "Maxxor").value("text", std::string{});
    }

    // One internal combat hit, shaped as the combat event log emits them.
    // localTime and gameTime are kept equal so a case can place an event on the
    // timeline and control its age with one number.
    json Hit(double at, std::string attacker, std::string target, bool namedAttacker = true)
    {
        json j;
        j["ne_kind"] = "hit";
        j["localTime"] = at;
        j["gameTime"] = at;
        j["originatingActorName"] = std::move(attacker);
        j["targetActorName"] = std::move(target);
        j["ne_actor_is_named"] = namedAttacker;
        return j;
    }

    // Any event that is not a hit, and therefore flushes a pending run.
    json Plain(double at, std::string type = "dialogue")
    {
        json j;
        j["type"] = std::move(type);
        j["localTime"] = at;
        j["gameTime"] = at;
        return j;
    }

    const json kNoEvents = json::array();
} // namespace

TEST_CASE("SkyrimNetEvents::FormatRelativeGameTime", "[SkyrimNetEvents]")
{
    SECTION("when the gap is under a minute")
    {
        SECTION("should say just now")
        {
            REQUIRE(Events::FormatRelativeGameTime(kMinute - 1.0) == "just now");
        }
    }

    SECTION("when the gap is exactly one minute")
    {
        SECTION("should say one minute ago")
        {
            // The bucket comparisons are all `<`, so the exact second a unit is
            // reached belongs to the larger unit. Pinned at every step below,
            // because an off-by-one here reads as plausible either way.
            REQUIRE(Events::FormatRelativeGameTime(kMinute) == "1 minute ago");
        }
    }

    SECTION("when the gap is several minutes")
    {
        SECTION("should pluralize minutes")
        {
            REQUIRE(Events::FormatRelativeGameTime(5.0 * kMinute) == "5 minutes ago");
        }
    }

    SECTION("when the gap reaches an hour")
    {
        SECTION("should switch to hours")
        {
            REQUIRE(Events::FormatRelativeGameTime(kHour) == "1 hour ago");
        }

        SECTION("should pluralize hours")
        {
            REQUIRE(Events::FormatRelativeGameTime(3.0 * kHour) == "3 hours ago");
        }
    }

    SECTION("when the gap reaches a day")
    {
        SECTION("should switch to days")
        {
            REQUIRE(Events::FormatRelativeGameTime(kDay) == "1 day ago");
        }

        SECTION("should pluralize days")
        {
            REQUIRE(Events::FormatRelativeGameTime(3.0 * kDay) == "3 days ago");
        }
    }

    SECTION("when the gap reaches a week")
    {
        SECTION("should switch to weeks")
        {
            REQUIRE(Events::FormatRelativeGameTime(kWeek) == "1 week ago");
        }

        SECTION("should pluralize weeks")
        {
            REQUIRE(Events::FormatRelativeGameTime(3.0 * kWeek) == "3 weeks ago");
        }
    }
}

TEST_CASE("SkyrimNetEvents::FormatRelativeGameDuration", "[SkyrimNetEvents]")
{
    SECTION("when the duration is under a minute")
    {
        SECTION("should say less than a minute")
        {
            // Not "just now": this variant is substituted into "for X" phrasing,
            // and "for just now" is not English.
            REQUIRE(Events::FormatRelativeGameDuration(kMinute - 1.0) == "less than a minute");
        }
    }

    SECTION("when the duration is exactly one minute")
    {
        SECTION("should say one minute with no ago")
        {
            REQUIRE(Events::FormatRelativeGameDuration(kMinute) == "1 minute");
        }
    }

    SECTION("when the duration is several minutes")
    {
        SECTION("should pluralize minutes")
        {
            REQUIRE(Events::FormatRelativeGameDuration(5.0 * kMinute) == "5 minutes");
        }
    }

    SECTION("when the duration reaches larger units")
    {
        SECTION("should switch to hours")
        {
            REQUIRE(Events::FormatRelativeGameDuration(2.0 * kHour) == "2 hours");
        }

        SECTION("should switch to days")
        {
            REQUIRE(Events::FormatRelativeGameDuration(kDay) == "1 day");
        }

        SECTION("should switch to weeks")
        {
            REQUIRE(Events::FormatRelativeGameDuration(kWeek) == "1 week");
        }
    }
}

TEST_CASE("SkyrimNetEvents::MemoryAgeGameSeconds", "[SkyrimNetEvents]")
{
    // Happy path, re-run per leaf: a well-formed memory row written an hour of
    // game time before the sampled clock. Each rejection case below breaks
    // exactly one of the preconditions.
    json row = json::object({{"game_time", kNow - kHour}});

    SECTION("when the row carries a usable game time")
    {
        SECTION("should return the elapsed game seconds")
        {
            // Game seconds, not real ones. The row's own `age_hours` field
            // measures wall-clock time since it was written, so a save resumed
            // after a two-month break reports every memory as ~60 days old
            // while none has aged an in-world hour -- which once made the
            // gossip harvester reject every candidate it looked at.
            REQUIRE(Events::MemoryAgeGameSeconds(row, kNow) == kHour);
        }
    }

    SECTION("when the row was written after the sampled clock")
    {
        // A row written a few simulation ticks ahead of the sampled clock is
        // "just now", not an error every caller has to handle.
        row["game_time"] = kNow + 500.0;

        SECTION("should clamp the age to zero")
        {
            REQUIRE(Events::MemoryAgeGameSeconds(row, kNow) == 0.0);
        }
    }

    SECTION("when the row is not an object")
    {
        SECTION("should report no usable age")
        {
            REQUIRE(Events::MemoryAgeGameSeconds(json("not a row"), kNow) < 0.0);
        }
    }

    SECTION("when the current clock is not positive")
    {
        SECTION("should report no usable age")
        {
            REQUIRE(Events::MemoryAgeGameSeconds(row, 0.0) < 0.0);
        }
    }

    SECTION("when the game_time field is missing")
    {
        row.erase("game_time");

        SECTION("should report no usable age")
        {
            REQUIRE(Events::MemoryAgeGameSeconds(row, kNow) < 0.0);
        }
    }

    SECTION("when the game_time field is not a number")
    {
        row["game_time"] = "96400";

        SECTION("should report no usable age")
        {
            REQUIRE(Events::MemoryAgeGameSeconds(row, kNow) < 0.0);
        }
    }

    SECTION("when the game_time field is not positive")
    {
        row["game_time"] = 0.0;

        SECTION("should report no usable age")
        {
            REQUIRE(Events::MemoryAgeGameSeconds(row, kNow) < 0.0);
        }
    }
}

TEST_CASE("SkyrimNetEvents::FormatEventsText renders known event types", "[SkyrimNetEvents]")
{
    SECTION("when the event is dialogue with a listener")
    {
        SECTION("should render speaker, listener and line")
        {
            const json event = json::object(
                {{"type", "dialogue"},
                 {"data",
                  json::object({{"speaker", "Ysolda"}, {"listener", "Carlotta"}, {"dialogue", "Good morning."}})}});
            REQUIRE(RenderOne(event) == "Ysolda -> Carlotta: \"Good morning.\"");
        }
    }

    SECTION("when the event is dialogue with no listener")
    {
        SECTION("should render speaker and line only")
        {
            const json event = json::object(
                {{"type", "dialogue"}, {"data", json::object({{"speaker", "Ysolda"}, {"dialogue", "Good morning."}})}});
            REQUIRE(RenderOne(event) == "Ysolda: \"Good morning.\"");
        }
    }

    SECTION("when the event is background dialogue")
    {
        SECTION("should render it like ordinary dialogue")
        {
            const json event = json::object(
                {{"type", "dialogue_background"},
                 {"data",
                  json::object({{"speaker", "Ysolda"}, {"listener", "Carlotta"}, {"dialogue", "Good morning."}})}});
            REQUIRE(RenderOne(event) == "Ysolda -> Carlotta: \"Good morning.\"");
        }
    }

    SECTION("when the event is player dialogue")
    {
        SECTION("should mark it as the player")
        {
            const json event = json::object(
                {{"type", "dialogue_player_text"},
                 {"data",
                  json::object({{"speaker", "Ysolda"}, {"listener", "Carlotta"}, {"dialogue", "Good morning."}})}});
            REQUIRE(RenderOne(event) == "(player) Ysolda -> Carlotta: \"Good morning.\"");
        }
    }

    SECTION("when the event is gamemaster dialogue")
    {
        SECTION("should render the topic too")
        {
            const json event = json::object({{"type", "gamemaster_dialogue"},
                                             {"data",
                                              json::object({{"speaker", "Ysolda"},
                                                            {"target", "Carlotta"},
                                                            {"topic", "the market"},
                                                            {"dialogue", "Move along."}})}});
            REQUIRE(RenderOne(event) == "Ysolda -> Carlotta (topic: the market): Move along.");
        }
    }

    SECTION("when the event is an npc thought")
    {
        SECTION("should render it as thinking")
        {
            const json event = json::object(
                {{"type", "npc_thoughts"},
                 {"data", json::object({{"npc_name", "Ysolda"}, {"thoughts", "That mammoth tusk again."}})}});
            REQUIRE(RenderOne(event) == "Ysolda (thinking): \"That mammoth tusk again.\"");
        }
    }

    SECTION("when the event is a death")
    {
        SECTION("should render killer and victim")
        {
            const json event =
                json::object({{"type", "death"}, {"data", json::object({{"killer", "Hans"}, {"victim", "Luke"}})}});
            REQUIRE(RenderOne(event) == "Hans killed Luke");
        }
    }

    SECTION("when the event is a persistent generic line")
    {
        SECTION("should render the line verbatim")
        {
            const json event = json::object({{"type", "persistent_generic"},
                                             {"data", json::object({{"line", "The bridge to Riverwood is out."}})}});
            REQUIRE(RenderOne(event) == "The bridge to Riverwood is out.");
        }
    }
}

TEST_CASE("SkyrimNetEvents::FormatEventsText handles book_read", "[SkyrimNetEvents]")
{
    // Happy path: a book event carrying every name field and a short body of
    // markup. Cases about title precedence erase the fields they are not
    // testing; cases about the body change the policy.
    json event = json::object({{"type", "book_read"},
                               {"data",
                                json::object({{"book_title", "The Lusty Argonian Maid"},
                                              {"book_name", "Lusty Argonian Maid, v1"},
                                              {"book_editor_id", "Book2CommonLustyArgonianMaid"},
                                              {"book_text", "<p><b>Chapter One</b></p>Lifts-Her-Tail"}})}});

    SECTION("when the book carries an in-fiction title")
    {
        SECTION("should name the book by that title")
        {
            REQUIRE(RenderOne(event) == "Maxxor read \"The Lusty Argonian Maid\"");
        }
    }

    SECTION("when only the record display name is present")
    {
        event["data"].erase("book_title");

        SECTION("should fall back to it")
        {
            REQUIRE(RenderOne(event) == "Maxxor read \"Lusty Argonian Maid, v1\"");
        }
    }

    SECTION("when only the editor id is present")
    {
        event["data"].erase("book_title");
        event["data"].erase("book_name");

        SECTION("should fall back to it")
        {
            // Last resort, so a nameless book still reads as something rather
            // than as "a book".
            REQUIRE(RenderOne(event) == "Maxxor read \"Book2CommonLustyArgonianMaid\"");
        }
    }

    SECTION("when the book has no name at all")
    {
        event["data"].erase("book_title");
        event["data"].erase("book_name");
        event["data"].erase("book_editor_id");

        SECTION("should say a book was read")
        {
            REQUIRE(RenderOne(event) == "Maxxor read a book");
        }
    }

    SECTION("when no player name is supplied")
    {
        SECTION("should call the reader the player")
        {
            const json out = FormatOne(event, kNow, BookTextPolicy::Omit, "");
            REQUIRE(out.value("text", std::string{}) == "The player read \"The Lusty Argonian Maid\"");
        }
    }

    SECTION("when the policy is Omit")
    {
        const json out = FormatOne(event, kNow, BookTextPolicy::Omit, "Maxxor");

        SECTION("should erase the body from the event data")
        {
            // The mutation matters on its own, separately from the rendered
            // line: the whole event object is serialized into the prompt
            // context, so raw markup left on `data` would cross the API
            // boundary even though no template renders it.
            REQUIRE_FALSE(out.at("data").contains("book_text"));
        }

        SECTION("should leave the body out of the rendered line")
        {
            REQUIRE(out.value("text", std::string{}) == "Maxxor read \"The Lusty Argonian Maid\"");
        }
    }

    SECTION("when the policy is Render")
    {
        const json out = FormatOne(event, kNow, BookTextPolicy::Render, "Maxxor");

        SECTION("should rewrite the body to plain text")
        {
            REQUIRE(out.at("data").at("book_text") == "Chapter One\nLifts-Her-Tail");
        }

        SECTION("should append the body to the rendered line")
        {
            REQUIRE(out.value("text", std::string{})
                    == "Maxxor read \"The Lusty Argonian Maid\":\nChapter One\nLifts-Her-Tail");
        }
    }

    SECTION("when the body is not a string")
    {
        event["data"]["book_text"] = 42;

        SECTION("should leave the body untouched under Render")
        {
            const json out = FormatOne(event, kNow, BookTextPolicy::Render, "Maxxor");
            REQUIRE(out.at("data").at("book_text") == 42);
        }

        SECTION("should still render the title line")
        {
            // Before the StringOr fix this threw type_error.302 and the caller
            // lost the entire batch rather than one unusable field.
            REQUIRE(RenderOne(event, BookTextPolicy::Render) == "Maxxor read \"The Lusty Argonian Maid\"");
        }
    }
}

TEST_CASE("SkyrimNetEvents::FormatEventsText survives malformed event data", "[SkyrimNetEvents]")
{
    // Happy path: an ordinary dialogue event. Each case below corrupts exactly
    // one field, the way a SkyrimNet payload or a third-party plugin does.
    json event = json::object(
        {{"type", "dialogue"},
         {"data", json::object({{"speaker", "Ysolda"}, {"listener", "Carlotta"}, {"dialogue", "Good morning."}})}});

    SECTION("when a field is present holding null")
    {
        event["data"]["speaker"] = nullptr;

        SECTION("should treat it as absent and render the rest")
        {
            // The headline case. SkyrimNet's own rows carry "display_name":null
            // and "condition_expr":null as a matter of course.
            REQUIRE(RenderOne(event) == " -> Carlotta: \"Good morning.\"");
        }
    }

    SECTION("when a field holds the wrong type")
    {
        event["data"]["speaker"] = 42;

        SECTION("should treat it as absent and render the rest")
        {
            REQUIRE(RenderOne(event) == " -> Carlotta: \"Good morning.\"");
        }
    }

    SECTION("when a field that steers the layout is null")
    {
        event["data"]["listener"] = nullptr;

        SECTION("should take the branch for an absent field")
        {
            // A null listener has to read as "no listener", not as a listener
            // whose name is empty, or the line gains a dangling arrow.
            REQUIRE(RenderOne(event) == "Ysolda: \"Good morning.\"");
        }
    }

    SECTION("when one event in a batch is malformed")
    {
        json events = json::array();
        events.push_back(event);
        json broken = event;
        broken["data"]["dialogue"] = nullptr;
        events.push_back(std::move(broken));
        events.push_back(
            json::object({{"type", "death"}, {"data", json::object({{"killer", "Hans"}, {"victim", "Luke"}})}}));
        Events::FormatEventsText(events, kNow, BookTextPolicy::Omit, "Maxxor");

        SECTION("should render every other event in the batch")
        {
            // The failure this guards: the throw used to escape mid-loop, so
            // events before the bad one kept their text, events after got none,
            // and the caller got an exception instead of a timeline.
            REQUIRE(events[0].at("text") == "Ysolda -> Carlotta: \"Good morning.\"");
            REQUIRE(events[2].at("text") == "Hans killed Luke");
        }

        SECTION("should render the malformed one with the field left empty")
        {
            REQUIRE(events[1].at("text") == "Ysolda -> Carlotta: \"\"");
        }
    }
}

TEST_CASE("SkyrimNetEvents::FormatEventsText falls back for unknown types", "[SkyrimNetEvents]")
{
    SECTION("when the payload is already a rendered sentence")
    {
        SECTION("should use it directly")
        {
            // Third-party SkyrimNet plugins emit events whose `data` is one
            // pre-rendered sentence, which is exactly the shape we want.
            const json event = json::object({{"type", "follower_left"}, {"data", "Lydia stopped following you."}});
            REQUIRE(RenderOne(event) == "Lydia stopped following you.");
        }
    }

    SECTION("when the payload is an object")
    {
        SECTION("should dump it as JSON")
        {
            const json event = json::object({{"type", "unheard_of"}, {"data", json::object({{"amount", 3}})}});
            REQUIRE(RenderOne(event) == "{\"amount\":3}");
        }
    }

    SECTION("when the payload is neither string nor container")
    {
        SECTION("should say there is no data")
        {
            const json event = json::object({{"type", "unheard_of"}, {"data", 42}});
            REQUIRE(RenderOne(event) == "(no data)");
        }
    }

    SECTION("when there is no payload at all")
    {
        SECTION("should say there is no data")
        {
            const json event = json::object({{"type", "unheard_of"}});
            REQUIRE(RenderOne(event) == "(no data)");
        }
    }

    SECTION("when a known type has no data object")
    {
        SECTION("should still render without throwing")
        {
            // Defensive path: the fields come back empty rather than the
            // renderer dereferencing a payload that is not there.
            const json event = json::object({{"type", "death"}});
            REQUIRE(RenderOne(event) == " killed ");
        }
    }

    SECTION("when the entry is not an object")
    {
        SECTION("should leave it untouched")
        {
            // A non-object entry is skipped outright, so no `text` is grafted
            // onto something that cannot carry one.
            const json out = FormatOne(json(42), kNow, BookTextPolicy::Omit, "Maxxor");
            REQUIRE(out == 42);
        }
    }
}

TEST_CASE("SkyrimNetEvents::FormatEventsText prefixes the relative time", "[SkyrimNetEvents]")
{
    json event =
        json::object({{"type", "persistent_generic"}, {"data", json::object({{"line", "The bridge is out."}})}});

    SECTION("when the event carries a game time")
    {
        event["gameTime"] = kNow - 5.0 * kMinute;

        SECTION("should prefix the bucketed age")
        {
            REQUIRE(RenderOne(event) == "[5 minutes ago] The bridge is out.");
        }
    }

    SECTION("when the event carries no game time")
    {
        SECTION("should render without a prefix")
        {
            REQUIRE(RenderOne(event) == "The bridge is out.");
        }
    }

    SECTION("when the event is timestamped in the future")
    {
        event["gameTime"] = kNow + 500.0;

        SECTION("should clamp the age to just now")
        {
            // A negative age would format as a nonsense bucket. Events can
            // legitimately arrive ahead of the sampled clock across a tick.
            REQUIRE(RenderOne(event) == "[just now] The bridge is out.");
        }
    }
}

TEST_CASE("SkyrimNetEvents::BuildMergedTimeline merges and filters", "[SkyrimNetEvents]")
{
    // Happy path: four empty-but-valid sources. Each case fills only the ones
    // it needs, which keeps the noise out of assertions about ordering. Every
    // event is placed recently enough that no idle marker is appended.
    json skyrimNet = json::array();
    json combat = json::array();
    json weather = json::array();
    json travel = json::array();

    SECTION("when every source has events")
    {
        skyrimNet.push_back(Plain(kNow - 40.0, "from_skyrimnet"));
        combat.push_back(Plain(kNow - 30.0, "from_combat"));
        weather.push_back(Plain(kNow - 20.0, "from_weather"));
        travel.push_back(Plain(kNow - 10.0, "from_travel"));

        SECTION("should include all of them")
        {
            const json out = Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow);
            REQUIRE(out.size() == 4);
        }
    }

    SECTION("when a source is not an array")
    {
        skyrimNet = json("not an array");
        combat.push_back(Plain(kNow - 10.0, "from_combat"));

        SECTION("should ignore it rather than throw")
        {
            const json out = Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].at("type") == "from_combat");
        }
    }

    SECTION("when an event is older than the age cap")
    {
        skyrimNet.push_back(Plain(kNow - 10.0, "fresh"));
        skyrimNet.push_back(Plain(kNow - 2.0 * kHour, "stale"));

        SECTION("should drop it")
        {
            // The cap is one game hour, and game time on purpose: a player who
            // slept through the night should not come back to a timeline still
            // describing last night's fight.
            const json out = Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].at("type") == "fresh");
        }
    }

    SECTION("when an event carries a null timestamp")
    {
        json bad = Plain(kNow - 10.0, "bad_time");
        bad["gameTime"] = nullptr;
        skyrimNet.push_back(bad);
        skyrimNet.push_back(Plain(kNow - 5.0, "good"));

        SECTION("should treat the timestamp as missing rather than throw")
        {
            // The numeric twin of the null-field defect above: reading
            // gameTime with nlohmann's value() threw type_error.302 on a
            // present-but-null key and took the entire merge down with it.
            REQUIRE_NOTHROW(Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow));
        }

        SECTION("should keep the well-formed events around it")
        {
            const json out = Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow);
            bool sawGood = false;
            for (const auto& e : out) {
                if (e.value("type", std::string{}) == "good")
                    sawGood = true;
            }
            REQUIRE(sawGood);
        }
    }

    SECTION("when an entry is not an object")
    {
        skyrimNet.push_back(42);
        skyrimNet.push_back(Plain(kNow - 10.0, "real"));

        SECTION("should drop it")
        {
            const json out = Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].at("type") == "real");
        }
    }

    SECTION("when events arrive out of order")
    {
        skyrimNet.push_back(Plain(kNow - 10.0, "later"));
        skyrimNet.push_back(Plain(kNow - 100.0, "earlier"));

        SECTION("should sort them by local time")
        {
            const json out = Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow);
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].at("type") == "earlier");
            REQUIRE(out[1].at("type") == "later");
        }
    }

    SECTION("when two events share a local time")
    {
        skyrimNet.push_back(Plain(kNow - 10.0, "first"));
        skyrimNet.push_back(Plain(kNow - 10.0, "second"));

        SECTION("should keep their input order")
        {
            // A stable sort, so SkyrimNet events keep preceding our internal
            // ones when they land on the same tick boundary.
            const json out = Events::BuildMergedTimeline(skyrimNet, combat, weather, travel, kNow);
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].at("type") == "first");
            REQUIRE(out[1].at("type") == "second");
        }
    }
}

TEST_CASE("SkyrimNetEvents::BuildMergedTimeline condenses combat hits", "[SkyrimNetEvents]")
{
    // Happy path: a combat array a case fills with hits, and a helper that runs
    // the merge and hands back the first entry's text. Every case here is about
    // what that summary says. Condensation is why the merge exists — a long
    // fight is hundreds of hit events, and unsummarized they crowd everything
    // else out of the prompt.
    json combat = json::array();
    const auto summaryText = [&]() -> std::string {
        const json out = Events::BuildMergedTimeline(kNoEvents, combat, kNoEvents, kNoEvents, kNow);
        REQUIRE_FALSE(out.empty());
        return out[0].value("text", std::string{});
    };

    SECTION("when two named actors trade a single blow")
    {
        combat.push_back(Hit(kNow - 10.0, "Hans", "Luke"));

        SECTION("should say one struck the other")
        {
            REQUIRE(summaryText() == "[just now] Hans strikes Luke");
        }
    }

    SECTION("when one actor lands repeated blows")
    {
        combat.push_back(Hit(kNow - 30.0, "Hans", "Luke"));
        combat.push_back(Hit(kNow - 20.0, "Hans", "Luke"));
        combat.push_back(Hit(kNow - 10.0, "Hans", "Luke"));

        SECTION("should say they attacked")
        {
            REQUIRE(summaryText() == "[just now] Hans attacks Luke");
        }
    }

    SECTION("when both actors land blows")
    {
        combat.push_back(Hit(kNow - 20.0, "Hans", "Luke"));
        combat.push_back(Hit(kNow - 10.0, "Luke", "Hans"));

        SECTION("should say they traded blows")
        {
            // The pair key is unordered, so Hans-hits-Luke and Luke-hits-Hans
            // land in one bucket rather than reading as two separate fights.
            REQUIRE(summaryText() == "[just now] Hans and Luke trade blows");
        }
    }

    SECTION("when several pairs are fighting")
    {
        combat.push_back(Hit(kNow - 60.0, "Alva", "Bran"));
        combat.push_back(Hit(kNow - 55.0, "Alva", "Bran"));
        combat.push_back(Hit(kNow - 50.0, "Alva", "Bran"));
        combat.push_back(Hit(kNow - 45.0, "Cira", "Dorn"));
        combat.push_back(Hit(kNow - 40.0, "Cira", "Dorn"));
        combat.push_back(Hit(kNow - 35.0, "Enar", "Fjori"));
        combat.push_back(Hit(kNow - 30.0, "Gorm", "Hilde"));

        SECTION("should summarize the busiest pairs")
        {
            // Only the two busiest are asserted: the sort is by hit count and
            // is not stable, so which of the single-hit pairs takes the third
            // slot is not a promise worth pinning.
            const std::string text = summaryText();
            REQUIRE(text.find("Combat continues: ") != std::string::npos);
            REQUIRE(text.find("Alva attacks Bran") != std::string::npos);
            REQUIRE(text.find("Cira attacks Dorn") != std::string::npos);
        }

        SECTION("should note that others were involved")
        {
            REQUIRE(summaryText().ends_with(", and others"));
        }
    }

    SECTION("when a hit has no named attacker")
    {
        combat.push_back(Hit(kNow - 10.0, "a bear trap", "Hans", false));

        SECTION("should read as damage taken")
        {
            REQUIRE(summaryText() == "[just now] Hans took damage from a bear trap");
        }

        SECTION("should say repeated when it happens more than once")
        {
            combat.push_back(Hit(kNow - 5.0, "a bear trap", "Hans", false));
            REQUIRE(summaryText() == "[just now] Hans took repeated damage from a bear trap");
        }
    }

    SECTION("when the environmental source is unlabelled")
    {
        combat.push_back(Hit(kNow - 10.0, "", "Hans", false));

        SECTION("should omit the source")
        {
            REQUIRE(summaryText() == "[just now] Hans took damage");
        }
    }

    SECTION("when actor and environmental hits are mixed")
    {
        combat.push_back(Hit(kNow - 20.0, "Hans", "Luke"));
        combat.push_back(Hit(kNow - 10.0, "a bear trap", "Ysolda", false));

        SECTION("should join both summaries")
        {
            REQUIRE(summaryText() == "[just now] Hans strikes Luke; Ysolda took damage from a bear trap");
        }
    }

    SECTION("when a non-hit event interrupts a run")
    {
        combat.push_back(Hit(kNow - 30.0, "Hans", "Luke"));
        combat.push_back(Plain(kNow - 20.0, "death"));
        combat.push_back(Hit(kNow - 10.0, "Hans", "Luke"));

        SECTION("should flush the run before it")
        {
            // Two separate summaries rather than one spanning the death, so a
            // reader can tell which blows landed before it.
            const json out = Events::BuildMergedTimeline(kNoEvents, combat, kNoEvents, kNoEvents, kNow);
            REQUIRE(out.size() == 3);
            REQUIRE(out[0].at("ne_kind") == "combat_summary");
            REQUIRE(out[1].at("type") == "death");
            REQUIRE(out[2].at("ne_kind") == "combat_summary");
        }
    }

    SECTION("when a run is condensed")
    {
        combat.push_back(Hit(kNow - 30.0, "Hans", "Luke"));
        combat.push_back(Hit(kNow - 10.0, "Hans", "Luke"));

        SECTION("should inherit the timestamps of its last hit")
        {
            // The newest hit's timestamps, so the summary sorts where the fight
            // actually ended rather than where it began.
            const json out = Events::BuildMergedTimeline(kNoEvents, combat, kNoEvents, kNoEvents, kNow);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].at("localTime") == kNow - 10.0);
            REQUIRE(out[0].at("gameTime") == kNow - 10.0);
        }
    }
}

TEST_CASE("SkyrimNetEvents::BuildMergedTimeline appends an idle marker", "[SkyrimNetEvents]")
{
    // The marker exists because without it the LLM re-reads the same stale
    // combat events tick after tick and keeps tension pinned high long after
    // the fight ended. It needs a concrete signal that the world went quiet.
    json skyrimNet = json::array();

    SECTION("when nothing survives the filter")
    {
        SECTION("should append a marker saying over an hour")
        {
            // The exact gap is unknowable here: the last event that could have
            // been measured against was just dropped for being too old.
            const json out = Events::BuildMergedTimeline(skyrimNet, kNoEvents, kNoEvents, kNoEvents, kNow);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].at("ne_kind") == "idle_marker");
            REQUIRE(out[0].at("text") == "Nothing notable has happened for over an hour");
        }
    }

    SECTION("when the newest event is stale")
    {
        skyrimNet.push_back(Plain(kNow - 11.0 * kMinute, "old_news"));

        SECTION("should append a marker naming the gap")
        {
            const json out = Events::BuildMergedTimeline(skyrimNet, kNoEvents, kNoEvents, kNoEvents, kNow);
            REQUIRE(out.size() == 2);
            REQUIRE(out[1].at("text") == "Nothing notable has happened for 11 minutes");
        }
    }

    SECTION("when the newest event is recent")
    {
        skyrimNet.push_back(Plain(kNow - 60.0, "recent"));

        SECTION("should append no marker")
        {
            const json out = Events::BuildMergedTimeline(skyrimNet, kNoEvents, kNoEvents, kNoEvents, kNow);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].at("type") == "recent");
        }
    }
}
