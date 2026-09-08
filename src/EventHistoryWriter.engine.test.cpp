#include <EventHistoryWriter.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <EventLogSpies.h>
#include <EventLogUtil.h>
#include <FakeSkyrimNet.h>
#include <PluginThread.h>
#include <SkyrimNetAPI.h>
#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
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
// would no longer be a record of the session. They are merged by time, stably,
// so events sharing an instant keep the order they were enqueued in.
//
// The file is written for real, under the log directory EngineMock reports, and
// every case reads back what a reader would see. The three internal drains are
// our own modules and are stood in for here.

namespace
{
    namespace EventHistoryWriter = NarrativeEngine::EventHistoryWriter;
    namespace EventLogUtil = NarrativeEngine::EventLogUtil;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    const std::filesystem::path kLogDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};
    const std::filesystem::path kHistoryFile = kLogDir / "NarrativeEngine_EventHistory.log";

    EventLogUtil::HistoryEntry Entry(double localTime, std::string kind, std::string body)
    {
        EventLogUtil::HistoryEntry e;
        e.localTime = localTime;
        e.inGameTimestamp = "[4E 201, Last Seed 17, 09:00:00]";
        e.sourceKind = std::move(kind);
        e.body = std::move(body);
        return e;
    }

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
        NarrativeEngine::Testing::EventLogSpies().combatQueue.clear();
        NarrativeEngine::Testing::EventLogSpies().weatherQueue.clear();
        NarrativeEngine::Testing::EventLogSpies().travelQueue.clear();
    }

    // The writer's flush is driven by the caller's tick accumulator, so a flush
    // is one Poll with enough elapsed time in it.
    void PollWith(double unpausedElapsedSeconds)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { EventHistoryWriter::Poll(pt, unpausedElapsedSeconds); });
    }
} // namespace

TEST_CASE("EventHistoryWriter::OnSessionStart", "[EventHistoryWriter][engine]")
{
    EngineMock engine;

    SECTION("when the archive is enabled")
    {
        const ConfiguredSettings settings{
            "[EventHistory]\nbEventHistoryEnabled=1\niEventHistoryFlushIntervalSeconds=5\n"};
        ClearHistoryFiles();
        EventHistoryWriter::OnSessionStart();
        NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
            Entry(1.0, "internal/combat_event", "Hans strikes Luke."));
        PollWith(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should open a file and write to it")
        {
            REQUIRE_FALSE(HistoryLines().empty());
        }
    }

    SECTION("when the archive is disabled")
    {
        const ConfiguredSettings settings{
            "[EventHistory]\nbEventHistoryEnabled=0\niEventHistoryFlushIntervalSeconds=5\n"};
        ClearHistoryFiles();
        EventHistoryWriter::OnSessionStart();

        SECTION("should write nothing at all")
        {
            // The master switch has to stop the file being opened, not merely
            // the lines being written: this archive grows without bound by
            // design, and a player who turned it off should get no file.
            NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
                Entry(1.0, "internal/combat_event", "Hans strikes Luke."));
            PollWith(60.0);
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
    const ConfiguredSettings settings{"[EventHistory]\nbEventHistoryEnabled=1\niEventHistoryFlushIntervalSeconds=5\n"};
    ClearHistoryFiles();
    EventHistoryWriter::OnSessionStart();
    NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
        Entry(1.0, "internal/combat_event", "Hans strikes Luke."));

    SECTION("when too little time has passed")
    {
        PollWith(1.0);

        SECTION("should hold the event rather than write it")
        {
            // Read while the session is still open. Closing it flushes
            // whatever is pending — which is right, and would hide this.
            REQUIRE(HistoryLines().empty());
        }

        SECTION("should still write it when the session closes")
        {
            // Nothing accumulated is dropped on the way out: a session that
            // ended between flushes is the common case, and losing its last
            // few seconds would lose exactly the events around a crash.
            EventHistoryWriter::OnSessionEnd();
            REQUIRE(HistoryLines().size() == 1);
        }
    }

    SECTION("when the interval elapses")
    {
        PollWith(6.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should write the event")
        {
            const auto lines = HistoryLines();
            REQUIRE(lines.size() == 1);
            REQUIRE(lines[0].find("Hans strikes Luke.") != std::string::npos);
        }

        SECTION("should prefix the line with the in-game time and the source")
        {
            // Absolute in-game time rather than the "[N ago]" the LLM sees:
            // this file is read against the player's account of when something
            // happened, and a relative stamp is meaningless once written down.
            const auto lines = HistoryLines();
            REQUIRE(lines[0].starts_with("[4E 201, Last Seed 17, 09:00:00] internal/combat_event: "));
        }
    }

    SECTION("when there is nothing to write")
    {
        NarrativeEngine::Testing::EventLogSpies().combatQueue.clear();
        PollWith(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should write nothing")
        {
            REQUIRE(HistoryLines().empty());
        }
    }

    SECTION("when no session is open")
    {
        EventHistoryWriter::OnSessionEnd();

        SECTION("should drain nothing and keep the events queued")
        {
            // Poll runs from the tick whether or not a save is loaded. Draining
            // here would consume events into a file nobody opened, and they
            // would never appear once one was.
            const auto queued = NarrativeEngine::Testing::EventLogSpies().combatQueue.size();
            PollWith(60.0);
            REQUIRE(NarrativeEngine::Testing::EventLogSpies().combatQueue.size() == queued);
        }
    }
}

TEST_CASE("EventHistoryWriter merges its four sources by time", "[EventHistoryWriter][engine]")
{
    // Each source keeps its own queue, so appending them one after another
    // would file every combat event before every weather event regardless of
    // when they happened.
    EngineMock engine;
    const ConfiguredSettings settings{"[EventHistory]\nbEventHistoryEnabled=1\niEventHistoryFlushIntervalSeconds=5\n"};
    ClearHistoryFiles();
    EventHistoryWriter::OnSessionStart();

    SECTION("when events arrive from several sources out of order")
    {
        NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(Entry(300.0, "internal/combat_event", "third"));
        NarrativeEngine::Testing::EventLogSpies().weatherQueue.push_back(
            Entry(100.0, "internal/weather_event", "first"));
        NarrativeEngine::Testing::EventLogSpies().travelQueue.push_back(
            Entry(200.0, "internal/travel_event", "second"));
        PollWith(60.0);
        EventHistoryWriter::OnSessionEnd();
        const auto lines = HistoryLines();

        SECTION("should write them in the order they happened")
        {
            REQUIRE(lines.size() == 3);
            REQUIRE(lines[0].find("first") != std::string::npos);
            REQUIRE(lines[1].find("second") != std::string::npos);
            REQUIRE(lines[2].find("third") != std::string::npos);
        }
    }

    SECTION("when two events share an instant")
    {
        NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
            Entry(100.0, "internal/combat_event", "combat at 100"));
        NarrativeEngine::Testing::EventLogSpies().weatherQueue.push_back(
            Entry(100.0, "internal/weather_event", "weather at 100"));
        PollWith(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should keep the order they were enqueued in")
        {
            // A stable sort, so a tie does not shuffle a causal pair into the
            // wrong order — which in an archive reads as the effect preceding
            // its cause.
            const auto lines = HistoryLines();
            REQUIRE(lines.size() == 2);
            REQUIRE(lines[0].find("combat at 100") != std::string::npos);
        }
    }

    SECTION("when a body spans several lines")
    {
        NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
            Entry(100.0, "internal/combat_event", "line one\nline two"));
        PollWith(60.0);
        EventHistoryWriter::OnSessionEnd();

        SECTION("should indent the continuation so one record stays one record")
        {
            // Book bodies are kept in full here, and they are long. Without
            // indentation a reader cannot tell where one record ends.
            const auto lines = HistoryLines();
            REQUIRE(lines.size() == 2);
            REQUIRE(lines[1].starts_with(" "));
        }
    }
}

TEST_CASE("EventHistoryWriter deduplicates SkyrimNet's stream", "[EventHistoryWriter][engine]")
{
    // SkyrimNet's events are fetched, not pushed, so the same ones come back on
    // every flush. Without a bookmark the archive fills with copies of
    // everything and stops being readable within a few minutes of play.
    EngineMock engine;
    const ConfiguredSettings settings{"[EventHistory]\nbEventHistoryEnabled=1\niEventHistoryFlushIntervalSeconds=5\n"};
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
        PollWith(60.0);
        PollWith(60.0);
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
        PollWith(60.0);
        SetJson(fake.eventsJson,
                R"([{"type":"dialogue","localTime":100.0,"gameTime":1.0,)"
                R"("data":{"speaker":"Ysolda","dialogue":"Good morning."}},)"
                R"({"type":"dialogue","localTime":200.0,"gameTime":2.0,)"
                R"("data":{"speaker":"Ysolda","dialogue":"Farewell."}}])");
        PollWith(60.0);
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
        NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
            Entry(1.0, "internal/combat_event", "Hans strikes Luke."));
        PollWith(60.0);
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
        NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
            Entry(1.0, "internal/combat_event", "Hans strikes Luke."));
        PollWith(60.0);
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
    const ConfiguredSettings settings{"[EventHistory]\nbEventHistoryEnabled=1\niEventHistoryFlushIntervalSeconds=5\n"};
    ClearHistoryFiles();

    SECTION("when a second session starts")
    {
        EventHistoryWriter::OnSessionStart();
        NarrativeEngine::Testing::EventLogSpies().combatQueue.push_back(
            Entry(1.0, "internal/combat_event", "first session"));
        PollWith(60.0);
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
