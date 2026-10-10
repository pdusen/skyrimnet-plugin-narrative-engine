#include <SenderDialogue.h>

#include <ConfiguredSettings.h>
#include <FakeSkyrimNet.h>
#include <SkyrimNetAPI.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Tests for the spoken history a compose prompt is given.
//
// The gate this module exists for is who a line was spoken TO. A letter or a
// visit is written in the voice of somebody who has been talking to the
// player, and the prompt says as much -- "the most recent times X and the
// player have actually spoken". Feed it the sender haggling with a merchant
// and it writes a letter continuing that conversation with the wrong person.
//
// That is why the fetch reads the event stream instead of SkyrimNet's
// dialogue endpoint: `originatingActorName` / `targetActorName` make the
// question answerable, where `{speaker, text, gameTime}` only makes it
// assumable. The cases below are built out of rows in the shape
// PublicGetRecentEvents documents, with a third party present in most of them
// so a filter that quietly passed everything could not pass.

namespace
{
    namespace Dialogue = NarrativeEngine::SenderDialogue;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    constexpr std::uint32_t kYsolda = 0x0001A6A0u;

    FakeSkyrimNetState& FakeState()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    template <std::size_t N> void SetJson(char (&dest)[N], const std::string& text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && i < text.size(); ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    // One row in the shape PublicGetRecentEvents actually returns: the
    // line nested under `data.dialogue`, with no top-level `text`.
    //
    // The first version of this helper emitted the flat `text` field that
    // PublicAPI.h's example shows, which let the whole suite pass against a
    // Fetch that dropped every real row on the floor. The fixture being
    // wrong in the same direction as the code is the only way a filter this
    // well covered could ship returning nothing, so the shape here is the
    // load-bearing part of these tests.
    std::string EventRow(const char* origin, const char* target, const char* line, double gameTime)
    {
        return R"({"type":"dialogue","gameTime":)" + std::to_string(gameTime) + R"(,"originatingActorName":")" + origin
               + R"(","targetActorName":")" + target + R"(","data":{"speaker":")" + origin + R"(","listener":")"
               + target + R"(","dialogue":")" + line + R"("}})";
    }

    // The same exchange in the rendered shape, which is what an array that
    // has already been through SkyrimNetEvents::FormatEventsText looks like.
    std::string FormattedEventRow(const char* origin, const char* target, const char* line, double gameTime)
    {
        return R"({"type":"dialogue","text":")" + std::string{line} + R"(","gameTime":)" + std::to_string(gameTime)
               + R"(,"originatingActorName":")" + origin + R"(","targetActorName":")" + target + R"("})";
    }

    std::string Rows(const std::vector<std::string>& rows)
    {
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i)
                out += ",";
            out += rows[i];
        }
        return out + "]";
    }

    std::vector<std::string> Texts(const nlohmann::json& dialogue)
    {
        std::vector<std::string> out;
        for (const auto& e : dialogue)
            out.push_back(e.value("text", ""));
        return out;
    }
} // namespace

TEST_CASE("SenderDialogue::Fetch keeps only what the two of them said to each other", "[SenderDialogue][engine]")
{
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    SECTION("when the sender and the player spoke to each other")
    {
        SetJson(fake.eventsJson,
                Rows({
                    EventRow("Dragonborn", "Ysolda", "Did the tusk ever turn up?", 100.0),
                    EventRow("Ysolda", "Dragonborn", "Not yet. Ask the Companions.", 110.0),
                }));

        SECTION("should keep both directions")
        {
            const auto out = Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10);
            REQUIRE(out.size() == 2);
            REQUIRE(Texts(out)
                    == std::vector<std::string>{"Did the tusk ever turn up?", "Not yet. Ask the Companions."});
        }

        SECTION("should name the speaker by the originating actor")
        {
            const auto out = Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10);
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].value("speaker", "") == "Dragonborn");
            REQUIRE(out[1].value("speaker", "") == "Ysolda");
        }

        SECTION("should carry gameTime through for the age filter")
        {
            // FilterByMemoryAge and AnnotateAges both read it; a row that
            // lost it would be treated as infinitely old and dropped.
            const auto out = Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10);
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].value("gameTime", 0.0) == 100.0);
        }
    }

    SECTION("when the sender was talking to somebody else")
    {
        SetJson(fake.eventsJson,
                Rows({
                    EventRow("Ysolda", "Carlotta Valentia", "The stall is yours tomorrow.", 100.0),
                    EventRow("Ysolda", "Dragonborn", "You again.", 110.0),
                }));

        SECTION("should drop the third-party line")
        {
            // The sender in a market is in dialogue rows with everyone
            // around them, and none of it is a conversation with the player.
            const auto out = Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].value("text", "") == "You again.");
        }
    }

    SECTION("when the player was talking to somebody else in front of the sender")
    {
        SetJson(fake.eventsJson,
                Rows({
                    EventRow("Dragonborn", "Carlotta Valentia", "How much for the apples?", 100.0),
                }));

        SECTION("should drop it")
        {
            // The event stream carries this row against the sender because
            // they were present, not because they were addressed.
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10).empty());
        }
    }

    SECTION("when two other people were talking to each other")
    {
        SetJson(fake.eventsJson,
                Rows({
                    EventRow("Carlotta Valentia", "Mila Valentia", "Inside, now.", 100.0),
                }));

        SECTION("should drop it")
        {
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10).empty());
        }
    }

    SECTION("when a row names no counterparty")
    {
        SetJson(fake.eventsJson,
                R"([{"type":"dialogue","text":"Someone muttered something.","gameTime":100.0,)"
                R"("originatingActorName":"Ysolda","targetActorName":null}])");

        SECTION("should drop it rather than assume the player")
        {
            // An unattributable line is exactly the one that must not reach
            // the prompt: assuming the player is how a remark to a third
            // party becomes something the sender said to them.
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10).empty());
        }
    }

    SECTION("when the row names the player by the literal \"Player\"")
    {
        SetJson(fake.eventsJson, Rows({EventRow("Ysolda", "Player", "You again.", 100.0)}));

        SECTION("should still recognise them")
        {
            // SkyrimNet's own documented example row carries "Player" rather
            // than the name the player chose.
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10).size() == 1);
        }
    }

    SECTION("when the names differ in case")
    {
        SetJson(fake.eventsJson, Rows({EventRow("YSOLDA", "dragonborn", "You again.", 100.0)}));

        SECTION("should still match them")
        {
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10).size() == 1);
        }
    }

    SECTION("when a name is spelled with a typographic apostrophe")
    {
        // Ma'randru-jo as SkyrimNet might hand the name back after a trip
        // through an LLM that curled the apostrophe. Both sides are
        // sanitized before comparison, so the trader does not lose their
        // whole history to one character.
        SetJson(fake.eventsJson,
                Rows({EventRow("Ma\xE2\x80\x99randru-jo", "Dragonborn", "Khajiit has wares.", 100.0)}));

        SECTION("should match the plain-apostrophe spelling")
        {
            const auto out = Dialogue::Fetch(kYsolda, "Ma'randru-jo", "Dragonborn", 10);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].value("speaker", "") == "Ma'randru-jo");
        }
    }

    SECTION("when the row has already been rendered to a flat text field")
    {
        SetJson(fake.eventsJson, Rows({FormattedEventRow("Ysolda", "Dragonborn", "You again.", 100.0)}));

        SECTION("should read the line from there")
        {
            // The fallback arm. Nothing in the plugin hands us a formatted
            // array today, but the header documents this shape and it costs
            // one lookup to accept it.
            const auto out = Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].value("text", "") == "You again.");
        }
    }

    SECTION("when a two-party row carries no line at all")
    {
        SetJson(fake.eventsJson,
                R"([{"type":"dialogue","gameTime":100.0,"originatingActorName":"Ysolda",)"
                R"("targetActorName":"Dragonborn","data":{"speaker":"Ysolda"}}])");

        SECTION("should drop it")
        {
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10).empty());
        }
    }

    SECTION("when a kept row has no text")
    {
        SetJson(fake.eventsJson,
                Rows({
                    EventRow("Ysolda", "Dragonborn", "", 100.0),
                    EventRow("Ysolda", "Dragonborn", "You again.", 110.0),
                }));

        SECTION("should drop the empty one")
        {
            const auto out = Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10);
            REQUIRE(out.size() == 1);
            REQUIRE(out[0].value("text", "") == "You again.");
        }
    }
}

TEST_CASE("SenderDialogue::Fetch orders and bounds what it returns", "[SenderDialogue][engine]")
{
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();

    SECTION("when the rows arrive out of order")
    {
        SetJson(fake.eventsJson,
                Rows({
                    EventRow("Ysolda", "Dragonborn", "third", 300.0),
                    EventRow("Ysolda", "Dragonborn", "first", 100.0),
                    EventRow("Ysolda", "Dragonborn", "second", 200.0),
                }));

        SECTION("should return them oldest first")
        {
            // The events endpoint documents no ordering, and the prompt
            // renders the block as a conversation, so the order is ours to
            // establish rather than to inherit.
            REQUIRE(Texts(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10))
                    == std::vector<std::string>{"first", "second", "third"});
        }
    }

    SECTION("when more rows survive than the cap allows")
    {
        SetJson(fake.eventsJson,
                Rows({
                    EventRow("Ysolda", "Dragonborn", "oldest", 100.0),
                    EventRow("Ysolda", "Dragonborn", "middle", 200.0),
                    EventRow("Ysolda", "Dragonborn", "newest", 300.0),
                }));

        SECTION("should keep the newest, not the first")
        {
            // Trimming from the other end would hand the prompt the oldest
            // exchanges on file and drop the one the sender is reacting to.
            REQUIRE(Texts(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 2))
                    == std::vector<std::string>{"middle", "newest"});
        }
    }

    SECTION("when asking SkyrimNet")
    {
        SetJson(fake.eventsJson, Rows({EventRow("Ysolda", "Dragonborn", "You again.", 100.0)}));
        (void)Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10);

        SECTION("should ask only for the event types that carry a spoken line")
        {
            REQUIRE(std::string{fake.lastEventFilter} == "dialogue,dialogue_player_text");
        }

        SECTION("should scope the query to the sender")
        {
            REQUIRE(fake.lastFormID == kYsolda);
        }

        SECTION("should over-fetch, because most rows will not survive the filter")
        {
            // Asking for exactly the cap would render two lines in a crowded
            // room when twenty survive further back.
            REQUIRE(fake.lastMaxCount > 10);
        }
    }
}

TEST_CASE("SenderDialogue::Fetch declines rather than guesses", "[SenderDialogue][engine]")
{
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    SetJson(fake.eventsJson, Rows({EventRow("Ysolda", "Dragonborn", "You again.", 100.0)}));

    SECTION("when the cap is zero")
    {
        SECTION("should return nothing and ask nothing")
        {
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 0).empty());
            REQUIRE(fake.recentEventsCalls == 0);
        }
    }

    SECTION("when the sender has no form")
    {
        SECTION("should return nothing")
        {
            REQUIRE(Dialogue::Fetch(0, "Ysolda", "Dragonborn", 10).empty());
        }
    }

    SECTION("when the sender has no name to match on")
    {
        SECTION("should return nothing")
        {
            // Every row would fail the name comparison anyway; bailing here
            // says so rather than looking like a sender nobody has met.
            REQUIRE(Dialogue::Fetch(kYsolda, "", "Dragonborn", 10).empty());
        }
    }

    SECTION("when SkyrimNet hands back something that is not an array")
    {
        SetJson(fake.eventsJson, R"({"error":"nope"})");

        SECTION("should return nothing")
        {
            REQUIRE(Dialogue::Fetch(kYsolda, "Ysolda", "Dragonborn", 10).empty());
        }
    }
}

TEST_CASE("SenderDialogue::FilterByMemoryAge", "[SenderDialogue][engine]")
{
    // The dialogue block and the memory block have to cover the same window.
    // A line from before the memory tail begins references events the prompt
    // has no memory context for, and reads as a past-life insert.
    const double now = 10000.0;

    auto dialogue = nlohmann::json::array();
    dialogue.push_back({{"speaker", "Ysolda"}, {"text", "old"}, {"gameTime", 1000.0}});
    dialogue.push_back({{"speaker", "Ysolda"}, {"text", "recent"}, {"gameTime", 9500.0}});

    SECTION("when the memory tail reaches back a known distance")
    {
        auto memories = nlohmann::json::array();
        memories.push_back({{"age_seconds", 1000.0}});

        SECTION("should drop dialogue from before it")
        {
            Dialogue::FilterByMemoryAge(dialogue, memories, now);
            REQUIRE(Texts(dialogue) == std::vector<std::string>{"recent"});
        }
    }

    SECTION("when the memory tail is empty")
    {
        SECTION("should keep everything")
        {
            // No lower bound to align to. Better the full window than a
            // block silently blanked by a missing field.
            Dialogue::FilterByMemoryAge(dialogue, nlohmann::json::array(), now);
            REQUIRE(dialogue.size() == 2);
        }
    }

    SECTION("when the memories carry no age")
    {
        auto memories = nlohmann::json::array();
        memories.push_back({{"content", "no age field"}});

        SECTION("should keep everything")
        {
            Dialogue::FilterByMemoryAge(dialogue, memories, now);
            REQUIRE(dialogue.size() == 2);
        }
    }

    SECTION("when there is no clock")
    {
        auto memories = nlohmann::json::array();
        memories.push_back({{"age_seconds", 1000.0}});

        SECTION("should keep everything")
        {
            // A zero clock would put the cutoff before the calendar epoch
            // and take every line with it.
            Dialogue::FilterByMemoryAge(dialogue, memories, 0.0);
            REQUIRE(dialogue.size() == 2);
        }
    }
}

TEST_CASE("SenderDialogue::AnnotateAges", "[SenderDialogue][engine]")
{
    auto dialogue = nlohmann::json::array();
    dialogue.push_back({{"speaker", "Ysolda"}, {"text", "line"}, {"gameTime", 9400.0}});

    SECTION("when the clock is known")
    {
        Dialogue::AnnotateAges(dialogue, 10000.0);

        SECTION("should render the age")
        {
            REQUIRE(dialogue[0].value("age_str", "") == "10 minutes ago");
        }

        SECTION("should drop the raw timestamp")
        {
            // The prompt renders age_str; leaving gameTime on the row spends
            // prompt budget on a number nothing reads.
            REQUIRE_FALSE(dialogue[0].contains("gameTime"));
        }
    }

    SECTION("when the clock is unknown")
    {
        Dialogue::AnnotateAges(dialogue, 0.0);

        SECTION("should say recent rather than invent an age")
        {
            REQUIRE(dialogue[0].value("age_str", "") == "recent");
        }
    }
}
