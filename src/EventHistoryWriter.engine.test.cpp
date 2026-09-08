#include <EventHistoryWriter.h>

#include <CombatEventLog.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <PluginThread.h>
#include <SkyrimNetAPI.h>
#include <ThreadRole.h>
#include <WeatherEventLog.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Tests for the session event-history file.
//
// This is the archive somebody reads when they want to know what actually
// happened in a session, and it is the only place the four event sources appear
// together. Three of them are ours; the fourth is SkyrimNet's own stream, which
// is fetched rather than pushed and therefore needs deduplication — the same
// events come back on every fetch, and without a bookmark the file fills with
// copies of everything until it is useless.
//
// Ordering matters as much as content. Each source keeps its own queue, so a
// flush that appended them one after another would file every combat event
// before every weather event regardless of when they happened, and the archive
// would no longer be a record of the session.
//
// Nothing here is stood in for. The internal logs are compiled into this
// executable for real, so the cases below produce their events the way the game
// does — a fight starting, the weather turning — and read back the file a
// player would attach to a bug report.

namespace
{
    namespace EventHistoryWriter = NarrativeEngine::EventHistoryWriter;
    namespace CombatEventLog = NarrativeEngine::CombatEventLog;
    namespace WeatherEventLog = NarrativeEngine::WeatherEventLog;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    const std::filesystem::path kLogDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};
    const std::filesystem::path kHistoryFile = kLogDir / "NarrativeEngine_EventHistory.log";

    // The archive on, with both internal logs responsive enough to produce an
    // event per driven poll.
    constexpr const char* kSettings =
        "[EventHistory]\nbEventHistoryEnabled=1\niEventHistoryFlushIntervalSeconds=5\n"
        "[CombatEvents]\niHitRadiusUnits=6000\niMaxStored=128\n"
        "[WeatherEvents]\niWeatherEventPollIntervalSeconds=1\niWeatherEventDebounceSeconds=0\n";

    FakeSkyrimNetState& FakeState()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    template <std::size_t N> void SetJson(char (&dest)[N], const char* text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && text[i] != '\0'; ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    // The RECORD lines only. The file opens with a legend, so counting every
    // line would count the header as content.
    std::vector<std::string> HistoryLines()
    {
        std::vector<std::string> lines;
        std::ifstream in{kHistoryFile};
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line.starts_with("#"))
                continue;
            lines.push_back(line);
        }
        return lines;
    }

    bool AnyLineContains(std::string_view needle)
    {
        const auto lines = HistoryLines();
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

    std::size_t RotatedFileCount()
    {
        std::size_t n = 0;
        std::error_code ec;
        for (int slot = 1; slot <= 5; ++slot) {
            if (std::filesystem::exists(kLogDir / ("NarrativeEngine_EventHistory." + std::to_string(slot) + ".log"),
                                        ec))
                ++n;
        }
        return n;
    }

    void ClearHistoryFiles()
    {
        EventHistoryWriter::OnSessionEnd();
        std::error_code ec;
        std::filesystem::remove(kHistoryFile, ec);
        for (int slot = 1; slot <= 5; ++slot) {
            std::filesystem::remove(kLogDir / ("NarrativeEngine_EventHistory." + std::to_string(slot) + ".log"), ec);
        }
        CombatEventLog::OnRevert();
        WeatherEventLog::OnRevert();
        (void)CombatEventLog::DrainHistoryTail();
        (void)WeatherEventLog::DrainHistoryTail();
    }

    void PollWriter(double unpausedElapsedSeconds)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { EventHistoryWriter::Poll(pt, unpausedElapsedSeconds); });
    }

    void PollCombat()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([](const PluginThread::Token& pt) { CombatEventLog::Poll(pt); });
    }

    void PollWeather()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [](const PluginThread::Token& pt) { WeatherEventLog::Poll(pt, 1.0); });
    }

    // Produces one combat event, the way a fight starting does.
    void StartAFight(EngineMock& engine)
    {
        engine.player.inCombat = false;
        PollCombat();
        engine.player.inCombat = true;
        PollCombat();
    }

    // Produces one weather event, the way the sky turning does.
    void TurnTheWeather(EngineMock& engine)
    {
        engine.sky.present = true;
        engine.sky.mode = static_cast<std::uint32_t>(RE::Sky::Mode::kFull);
        engine.sky.hasWeather = true;
        engine.sky.thunderLightningFrequency = 0;
        engine.sky.windSpeed = 0;
        engine.sky.weatherFlags = 0x01; // pleasant
        engine.sky.weatherFormID = 0x0010E1F2u;
        PollWeather();
        engine.sky.weatherFlags = 0x04; // rainy
        engine.sky.weatherFormID = 0x0010E1F3u;
        PollWeather();
    }
} // namespace

TEST_CASE("EventHistoryWriter::OnSessionStart", "[EventHistoryWriter][engine]")
{
    EngineMock engine;

    SECTION("when the archive is enabled")
    {
        const ConfiguredSettings settings{kSettings};
        ClearHistoryFiles();
        EventHistoryWriter::OnSessionStart();
        StartAFight(engine);
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should open a file and write to it")
        {
            REQUIRE_FALSE(HistoryLines().empty());
        }
    }

    SECTION("when the archive is disabled")
    {
        const ConfiguredSettings settings{"[EventHistory]\nbEventHistoryEnabled=0\n"
                                          "iEventHistoryFlushIntervalSeconds=5\n"
                                          "[CombatEvents]\niHitRadiusUnits=6000\n"};
        ClearHistoryFiles();
        EventHistoryWriter::OnSessionStart();

        SECTION("should write nothing at all")
        {
            // The master switch has to stop the file being opened, not merely
            // the lines being written: this archive grows without bound by
            // design, and a player who turned it off should get no file.
            StartAFight(engine);
            PollWriter(60.0);
            REQUIRE(HistoryLines().empty());
        }

        SECTION("should leave the previous session's file alone")
        {
            // No rotation either. Turning the archive off must not quietly
            // discard the session somebody was about to send in.
            REQUIRE(RotatedFileCount() == 0);
        }
    }
}

TEST_CASE("EventHistoryWriter flushes on the tick accumulator", "[EventHistoryWriter][engine]")
{
    // Happy path, re-run per leaf: the archive on, a five-second flush
    // interval, and one combat event waiting.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    ClearHistoryFiles();
    EventHistoryWriter::OnSessionStart();
    StartAFight(engine);

    SECTION("when too little time has passed")
    {
        PollWriter(1.0);

        SECTION("should hold the event rather than write it")
        {
            // Read while the session is still open. Closing it flushes whatever
            // is pending — which is right, and would hide this.
            REQUIRE(HistoryLines().empty());
        }

        SECTION("should still write it when the session closes")
        {
            // Nothing accumulated is dropped on the way out: a session that
            // ended between flushes is the common case, and losing its last few
            // seconds would lose exactly the events around a crash.
            EventHistoryWriter::OnSessionEnd();
            REQUIRE(HistoryLines().size() == 1);
        }
    }

    SECTION("when the interval elapses")
    {
        PollWriter(6.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should write the event")
        {
            const auto lines = HistoryLines();
            REQUIRE(lines.size() == 1);
            REQUIRE(lines[0].find("internal/combat_event/") != std::string::npos);
        }

        SECTION("should prefix the line with the in-game time")
        {
            // Absolute in-game time rather than the "[N ago]" the LLM sees:
            // this file is read against the player's account of when something
            // happened, and a relative stamp is meaningless once written down.
            const auto lines = HistoryLines();
            REQUIRE(lines.size() == 1);
            REQUIRE(lines[0].starts_with("[4E "));
        }
    }

    SECTION("when there is nothing to write")
    {
        (void)CombatEventLog::DrainHistoryTail();
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should write nothing")
        {
            REQUIRE(HistoryLines().empty());
        }
    }

    SECTION("when no session is open")
    {
        EventHistoryWriter::OnSessionEnd();
        ClearHistoryFiles();
        StartAFight(engine);

        SECTION("should drain nothing and keep the events queued")
        {
            // Poll runs from the tick whether or not a save is loaded. Draining
            // here would consume events into a file nobody opened, and they
            // would never appear once one was.
            PollWriter(60.0);
            REQUIRE(CombatEventLog::DrainHistoryTail().size() == 1);
        }
    }
}

TEST_CASE("EventHistoryWriter merges its sources by time", "[EventHistoryWriter][engine]")
{
    // Each source keeps its own queue, so appending them one after another
    // would file every combat event before every weather event regardless of
    // when they happened.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    ClearHistoryFiles();
    EventHistoryWriter::OnSessionStart();

    SECTION("when two internal sources have events")
    {
        TurnTheWeather(engine);
        StartAFight(engine);
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();
        const auto lines = HistoryLines();

        SECTION("should write both")
        {
            REQUIRE(lines.size() == 2);
            REQUIRE(AnyLineContains("internal/weather_event/"));
            REQUIRE(AnyLineContains("internal/combat_event/"));
        }

        SECTION("should write them in the order they happened")
        {
            // The weather turned before the fight started, and the archive has
            // to say so — a reader uses this file to reconstruct a sequence,
            // and each source's queue is drained whole.
            REQUIRE(lines.size() == 2);
            REQUIRE(lines[0].find("internal/weather_event/") != std::string::npos);
        }
    }
}

TEST_CASE("EventHistoryWriter deduplicates SkyrimNet's stream", "[EventHistoryWriter][engine]")
{
    // SkyrimNet's events are fetched, not pushed, so the same ones come back on
    // every flush. Without a bookmark the archive fills with copies of
    // everything and stops being readable within a few minutes of play.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    ClearHistoryFiles();
    EventHistoryWriter::OnSessionStart();
    // A real SkyrimNet event shape: the text is SYNTHESIZED from `type` and
    // `data` rather than taken from any `text` field, so a stripped-down
    // fixture would render as an empty line and pass nothing.
    SetJson(fake.eventsJson,
            R"([{"type":"dialogue","localTime":100.0,"gameTime":1.0,)"
            R"("data":{"speaker":"Ysolda","dialogue":"Good morning."}}])");

    SECTION("when the same events come back on the next flush")
    {
        PollWriter(60.0);
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should write each event once")
        {
            const auto lines = HistoryLines();
            const auto copies = std::count_if(lines.begin(), lines.end(), [](const std::string& line) {
                return line.find("Good morning.") != std::string::npos;
            });
            REQUIRE(copies == 1);
        }
    }

    SECTION("when a newer event arrives")
    {
        PollWriter(60.0);
        SetJson(fake.eventsJson,
                R"([{"type":"dialogue","localTime":100.0,"gameTime":1.0,)"
                R"("data":{"speaker":"Ysolda","dialogue":"Good morning."}},)"
                R"({"type":"dialogue","localTime":200.0,"gameTime":2.0,)"
                R"("data":{"speaker":"Ysolda","dialogue":"Farewell."}}])");
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should write only the new one")
        {
            const auto lines = HistoryLines();
            REQUIRE(lines.size() == 2);
            REQUIRE(lines[1].find("Farewell.") != std::string::npos);
        }
    }

    SECTION("when SkyrimNet answers with nothing")
    {
        SetJson(fake.eventsJson, "[]");
        StartAFight(engine);
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should still write the internal events")
        {
            // One source having nothing to say must not cost the others their
            // flush.
            REQUIRE(HistoryLines().size() == 1);
        }
    }

    SECTION("when SkyrimNet answers with something unparseable")
    {
        SetJson(fake.eventsJson, "not json at all");
        StartAFight(engine);
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should still write the internal events")
        {
            REQUIRE(HistoryLines().size() == 1);
        }
    }
}

TEST_CASE("EventHistoryWriter rotates its files", "[EventHistoryWriter][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    ClearHistoryFiles();

    SECTION("when a second session starts")
    {
        EventHistoryWriter::OnSessionStart();
        StartAFight(engine);
        PollWriter(60.0);
        EventHistoryWriter::OnSessionEnd();
        EventHistoryWriter::OnSessionStart();

        SECTION("should keep the previous session")
        {
            // The report almost always arrives after a restart, so the session
            // that went wrong is the previous one.
            EventHistoryWriter::OnSessionEnd();
            REQUIRE(RotatedFileCount() >= 1);
        }

        SECTION("should start the new file empty")
        {
            EventHistoryWriter::OnSessionEnd();
            REQUIRE(HistoryLines().empty());
        }
    }

    SECTION("when more sessions start than there are slots")
    {
        SECTION("should keep no more than the slot count")
        {
            // Bounded on purpose: this file takes every event in the session,
            // book bodies included, and an unbounded set would fill a disk.
            for (int i = 0; i < 8; ++i) {
                EventHistoryWriter::OnSessionStart();
                EventHistoryWriter::OnSessionEnd();
            }
            REQUIRE(RotatedFileCount() <= 5);
        }
    }
}
