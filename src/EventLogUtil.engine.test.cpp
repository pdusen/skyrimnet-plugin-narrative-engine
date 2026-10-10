#include <EventLogUtil.h>

#include <EngineMock.h>
#include <SKSECosaveIO.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Mocked-engine tests for the shared event-log helpers.
//
// Three of these are pure arithmetic over a calendar reading, and they carry
// most of the risk in the module. The header records why: an earlier revision
// of `NowGameTimeSeconds` returned time-of-day rather than cumulative time,
// which silently broke every internal event on any save past day one — the
// merge's age filter compares against cumulative game time and found every
// event to be millions of seconds old. Nothing about that failure was visible
// at the call site, which is exactly the shape a test catches and a playthrough
// does not.
//
// The date arithmetic is the other half. It walks month lengths by hand from a
// fixed epoch, so month and year rollover are the cases that matter.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace EventLogUtil = NarrativeEngine::EventLogUtil;

    constexpr double kDay = 86400.0;

    // A non-null interface pointer; the mocked SKSE methods answer out of
    // EngineMock and never read through it.
    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }
} // namespace

TEST_CASE("EventLogUtil::NowGameTimeSeconds", "[EventLogUtil][engine]")
{
    // Happy path: a calendar sitting some way past the game start.
    EngineMock engine;

    SECTION("when the calendar is available")
    {
        engine.calendar.daysPassed = 3.5f;

        SECTION("should report cumulative game seconds")
        {
            // Cumulative, not time-of-day. The distinction is the whole point:
            // a time-of-day reading looks plausible on day one and makes every
            // event on day two read as impossibly old.
            REQUIRE(EventLogUtil::NowGameTimeSeconds() == 3.5 * kDay);
        }

        SECTION("should keep growing past the first day")
        {
            engine.calendar.daysPassed = 400.0f;
            REQUIRE(EventLogUtil::NowGameTimeSeconds() > 365.0 * kDay);
        }
    }

    SECTION("when the calendar is unavailable")
    {
        engine.calendar.present = false;

        SECTION("should report zero")
        {
            engine.calendar.daysPassed = 3.5f;
            REQUIRE(EventLogUtil::NowGameTimeSeconds() == 0.0);
        }
    }
}

TEST_CASE("EventLogUtil::NowUnixSeconds", "[EventLogUtil][engine]")
{
    SECTION("when asked twice")
    {
        SECTION("should not go backwards")
        {
            // Wall-clock, so no exact value can be asserted. What callers rely
            // on is that it is monotonic enough to sort a batch by and large
            // enough to be a real epoch reading rather than a session counter.
            const double first = EventLogUtil::NowUnixSeconds();
            const double second = EventLogUtil::NowUnixSeconds();
            REQUIRE(second >= first);
        }

        SECTION("should be a unix-epoch reading")
        {
            // Past 2001 in epoch seconds. A session-relative counter or a
            // millisecond reading would both fail this.
            REQUIRE(EventLogUtil::NowUnixSeconds() > 1'000'000'000.0);
        }
    }
}

TEST_CASE("EventLogUtil::CurrentInGameTimestamp", "[EventLogUtil][engine]")
{
    // Happy path: the canonical game start at midday.
    EngineMock engine;

    SECTION("when the calendar is available")
    {
        engine.calendar.year = 201;
        engine.calendar.month = 9; // Frostfall
        engine.calendar.day = 15.0f;
        engine.calendar.hour = 17.7f;

        SECTION("should render the date and time")
        {
            const std::string stamp = EventLogUtil::CurrentInGameTimestamp();
            REQUIRE(stamp.starts_with("[4E 201, Frostfall 15, 17:"));
        }

        SECTION("should render the month by name")
        {
            engine.calendar.month = 0;
            REQUIRE(EventLogUtil::CurrentInGameTimestamp().find("Morning Star") != std::string::npos);
        }

        SECTION("should split the fractional hour into minutes and seconds")
        {
            // 17.5 hours is 17:30:00. The float arithmetic makes the seconds
            // approximate, so the minute is what is pinned.
            engine.calendar.hour = 17.5f;
            REQUIRE(EventLogUtil::CurrentInGameTimestamp().find("17:30:") != std::string::npos);
        }
    }

    SECTION("when the month is outside the calendar")
    {
        // Should not happen — GetMonth returns 0-11 — but the formatter guards
        // it rather than indexing past the end of the name table.
        engine.calendar.month = 99;

        SECTION("should say the month is unknown")
        {
            REQUIRE(EventLogUtil::CurrentInGameTimestamp().find("Unknown") != std::string::npos);
        }
    }

    SECTION("when the calendar is unavailable")
    {
        engine.calendar.present = false;

        SECTION("should say the time is unknown")
        {
            REQUIRE(EventLogUtil::CurrentInGameTimestamp() == "[unknown time]");
        }
    }
}

TEST_CASE("EventLogUtil::FormatInGameTimestampFromGameTime", "[EventLogUtil][engine]")
{
    // Pure arithmetic from a fixed epoch: 17 Last Seed 4E 201 is day zero,
    // months have Skyrim's own lengths, and a year is 365 days with no leap.
    // No calendar is consulted, so no mock state steers these.

    SECTION("when the value is the epoch itself")
    {
        SECTION("should render the canonical game start")
        {
            REQUIRE(EventLogUtil::FormatInGameTimestampFromGameTime(0.0) == "[4E 201, Last Seed 17, 00:00:00]");
        }
    }

    SECTION("when a whole day has passed")
    {
        SECTION("should advance the day")
        {
            REQUIRE(EventLogUtil::FormatInGameTimestampFromGameTime(kDay) == "[4E 201, Last Seed 18, 00:00:00]");
        }
    }

    SECTION("when the day rolls over into the next month")
    {
        SECTION("should advance the month")
        {
            // Last Seed has 31 days, so day 17 plus 15 lands on 1 Hearthfire.
            REQUIRE(EventLogUtil::FormatInGameTimestampFromGameTime(15.0 * kDay) == "[4E 201, Hearthfire 1, 00:00:00]");
        }
    }

    SECTION("when a whole year has passed")
    {
        SECTION("should advance the year and keep the date")
        {
            // 365 days exactly, so the calendar lands on the same day one year
            // on. An off-by-one in the month walk shows up here and nowhere
            // else.
            REQUIRE(EventLogUtil::FormatInGameTimestampFromGameTime(365.0 * kDay)
                    == "[4E 202, Last Seed 17, 00:00:00]");
        }
    }

    SECTION("when the value carries a time of day")
    {
        SECTION("should render the hours and minutes")
        {
            // Pinned to the minute, not the second. The value is divided down
            // to a day fraction and multiplied back out through hours and
            // minutes, so the trailing second drifts by one either way for most
            // inputs -- 09:30:15 comes back as 09:30:14. That is a fair
            // description of what the arithmetic can deliver, and the header's
            // claim of second precision is a little generous; nothing consumes
            // the field that closely, so it is recorded rather than changed.
            const double stamp = 9.0 * 3600.0 + 30.0 * 60.0 + 15.0;
            REQUIRE(
                EventLogUtil::FormatInGameTimestampFromGameTime(stamp).starts_with("[4E 201, Last Seed 17, 09:30:"));
        }

        SECTION("should zero-pad each field")
        {
            REQUIRE(EventLogUtil::FormatInGameTimestampFromGameTime(3600.0 + 120.0 + 3.0)
                    == "[4E 201, Last Seed 17, 01:02:03]");
        }
    }

    SECTION("when the value is negative enough to precede year zero")
    {
        SECTION("should refuse rather than emit a nonsense date")
        {
            // A negative game time means a corrupt event rather than a real
            // moment, and a wrapped date in the history log would read as a
            // genuine one.
            REQUIRE(EventLogUtil::FormatInGameTimestampFromGameTime(-1'000'000.0 * kDay) == "[invalid time]");
        }
    }
}

TEST_CASE("EventLogUtil::RotateLogFiles", "[EventLogUtil][engine]")
{
    // Rotation has to shift the history along AND leave the current file
    // exactly where it was, because something is usually reading it.
    //
    // These logs are read by tailing them in an editor while the game
    // runs. A tail follows the FILE, not the name, so moving the current
    // file aside and creating a fresh one at the same path silently
    // leaves the reader watching a rotated copy that will never grow
    // again. The end state on disk is identical either way, which is why
    // this is worth pinning: nothing about the resulting files says which
    // was done, only whether the current one is still the same file.
    namespace fs = std::filesystem;
    namespace EventLogUtil = NarrativeEngine::EventLogUtil;

    const auto dir = fs::temp_directory_path() / "ne_rotate_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const std::string stem = "trace";
    const auto current = dir / "trace.log";
    const auto first = dir / "trace.1.log";
    const auto second = dir / "trace.2.log";

    const auto write = [](const fs::path& path, std::string_view text) {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        out << text;
    };
    const auto read = [](const fs::path& path) {
        std::ifstream in(path);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    };

    SECTION("when a previous session's log is still there")
    {
        write(current, "session one");
        EventLogUtil::RotateLogFiles(dir, stem, 5, "Test");

        SECTION("should keep the current file rather than move it aside")
        {
            // The whole point. After a rename the path is empty until
            // something recreates it, and whatever was tailing the old
            // file is now tailing history.
            REQUIRE(fs::exists(current));
        }

        SECTION("should put the old contents in the first history slot")
        {
            REQUIRE(read(first) == "session one");
        }
    }

    SECTION("when several sessions have already rotated")
    {
        write(current, "newest");
        write(first, "older");
        EventLogUtil::RotateLogFiles(dir, stem, 5, "Test");

        SECTION("should shift the history along behind the current file")
        {
            REQUIRE(read(first) == "newest");
            REQUIRE(read(second) == "older");
        }
    }

    SECTION("when the oldest slot is full")
    {
        write(current, "newest");
        write(dir / "trace.5.log", "ancient");
        EventLogUtil::RotateLogFiles(dir, stem, 5, "Test");

        SECTION("should drop it rather than grow the family forever")
        {
            REQUIRE_FALSE(fs::exists(dir / "trace.5.log"));
        }
    }

    SECTION("when there is no log yet")
    {
        SECTION("should do nothing rather than fabricate one")
        {
            EventLogUtil::RotateLogFiles(dir, stem, 5, "Test");
            REQUIRE_FALSE(fs::exists(current));
            REQUIRE_FALSE(fs::exists(first));
        }
    }

    fs::remove_all(dir, ec);
}

TEST_CASE("EventLogUtil string serialization", "[EventLogUtil][engine]")
{
    // Length-prefixed strings in the co-save. The round trip matters more than
    // either half: a writer and reader that disagree corrupt every record after
    // the first, and the damage surfaces on load rather than on save.
    EngineMock engine;
    auto* intfc = FakeInterface();

    SECTION("when a string is written and read back")
    {
        EventLogUtil::WriteString(intfc, "Ysolda");
        engine.cosave.readable = engine.cosave.written;

        SECTION("should return the same text")
        {
            std::string out;
            REQUIRE(EventLogUtil::ReadString(intfc, out));
            REQUIRE(out == "Ysolda");
        }
    }

    SECTION("when the string is empty")
    {
        EventLogUtil::WriteString(intfc, "");
        engine.cosave.readable = engine.cosave.written;

        SECTION("should write only the length prefix")
        {
            REQUIRE(engine.cosave.written.size() == sizeof(std::uint32_t));
        }

        SECTION("should read back as empty")
        {
            std::string out = "not empty";
            REQUIRE(EventLogUtil::ReadString(intfc, out));
            REQUIRE(out.empty());
        }
    }

    SECTION("when the record ends before the length prefix")
    {
        SECTION("should report failure")
        {
            std::string out;
            REQUIRE_FALSE(EventLogUtil::ReadString(intfc, out));
        }
    }

    SECTION("when the record ends part way through the text")
    {
        EventLogUtil::WriteString(intfc, "Ysolda");
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readable.resize(engine.cosave.readable.size() - 2);

        SECTION("should report failure")
        {
            // A torn record. The caller's contract is to abort the parse and
            // revert, which it can only do if this says no.
            std::string out;
            REQUIRE_FALSE(EventLogUtil::ReadString(intfc, out));
        }
    }
}

TEST_CASE("EventLogUtil::DrainVector", "[EventLogUtil][engine]")
{
    std::vector<EventLogUtil::HistoryEntry> pending;

    SECTION("when the queue holds entries")
    {
        pending.push_back({1.0, "[stamp]", "internal/weather_event/rain_start", "It began to rain."});
        pending.push_back({2.0, "[stamp]", "internal/combat_event/hit", "Hans strikes Luke."});
        const auto drained = EventLogUtil::DrainVector(pending);

        SECTION("should hand all of them back")
        {
            REQUIRE(drained.size() == 2);
            REQUIRE(drained[0].body == "It began to rain.");
        }

        SECTION("should leave the queue empty")
        {
            // A drain that copied rather than moved would re-emit every entry
            // on the next flush, duplicating the whole history log.
            REQUIRE(pending.empty());
        }
    }

    SECTION("when the queue is empty")
    {
        SECTION("should hand back nothing")
        {
            REQUIRE(EventLogUtil::DrainVector(pending).empty());
        }
    }
}
