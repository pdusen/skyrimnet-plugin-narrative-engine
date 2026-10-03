#include <VisitorTravelLog.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Tests for the visitor-travel trace.
//
// The file exists because every arrival defect found so far was a decision
// that read correctly in summary and wrongly in detail, and diagnosing each
// one cost either a hand simulation or another play session. A trace that
// silently stops being written puts the next one back to that, so the things
// worth pinning here are the ones that fail quietly: the file not opening, the
// enable flag not being honoured, rotation losing the session a tester is
// about to attach to a report, and lines being dropped because nothing noticed
// the stream was closed.
//
// Exercised for real rather than mocked: it writes a genuine file under the
// directory EngineMock reports as SKSE's log directory, and each case reads
// back what a reader would open. Rotation is a filesystem behaviour and cannot
// be checked any other way.

namespace
{
    namespace VisitorTravelLog = NarrativeEngine::VisitorTravelLog;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    const std::filesystem::path kLogDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};

    std::filesystem::path TracePath(int slot = 0)
    {
        if (slot == 0) {
            return kLogDir / "NarrativeEngine_VisitorTravel.log";
        }
        return kLogDir / ("NarrativeEngine_VisitorTravel." + std::to_string(slot) + ".log");
    }

    std::vector<std::string> TraceLines(int slot = 0)
    {
        std::vector<std::string> lines;
        std::ifstream in{TracePath(slot)};
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    bool AnyLineContains(std::string_view needle, int slot = 0)
    {
        const auto lines = TraceLines(slot);
        return std::any_of(lines.begin(), lines.end(), [needle](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

    void ClearTrace()
    {
        VisitorTravelLog::OnSessionEnd();
        std::error_code ec;
        for (int slot = 0; slot <= 5; ++slot) {
            std::filesystem::remove(TracePath(slot), ec);
        }
    }

    constexpr const char* kOn = "[Beats]\nbVisitorTravelLogEnabled=true\n";
    constexpr const char* kOff = "[Beats]\nbVisitorTravelLogEnabled=false\n";
} // namespace

TEST_CASE("VisitorTravelLog writes a visit transcript", "[VisitorTravelLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kOn};
    ClearTrace();

    SECTION("when a session has started")
    {
        VisitorTravelLog::OnSessionStart();

        SECTION("should report itself active")
        {
            REQUIRE(VisitorTravelLog::IsActive());
            VisitorTravelLog::OnSessionEnd();
        }

        SECTION("should frame a visit by sender and name")
        {
            // One beat runs at a time, so the frame is what makes the file
            // readable without filtering: everything between BEGIN and END
            // belongs to one visitor.
            VisitorTravelLog::Begin(0x0001C1A9u, "Faralda");
            VisitorTravelLog::End(0x0001C1A9u, "chain", "fine");
            VisitorTravelLog::OnSessionEnd();

            REQUIRE(AnyLineContains("BEGIN"));
            REQUIRE(AnyLineContains("0x0001C1A9"));
            REQUIRE(AnyLineContains("Faralda"));
            REQUIRE(AnyLineContains("tier=chain class=fine"));
        }

        SECTION("should write the detail a reader came for")
        {
            // The tag column is the point: a reader skims one tag down the
            // file — every GATE to see what was refused, every COVER to see
            // which ray failed.
            VisitorTravelLog::Write(
                "GATE", "({:.0f},{:.0f}) rejected: {:.0f}u is inside the {:.0f}u floor", 1.0f, 2.0f, 1900.0f, 2000.0f);
            VisitorTravelLog::OnSessionEnd();

            const auto lines = TraceLines();
            const auto hit = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
                return line.find("GATE") != std::string::npos;
            });
            REQUIRE(hit != lines.end());
            REQUIRE(hit->find("1900u is inside the 2000u floor") != std::string::npos);
        }

        SECTION("should name a visit that never reached a tier")
        {
            // A decline is the case most worth tracing, since the summary
            // line says only that nothing was found.
            VisitorTravelLog::Begin(0x00001234u, "Nelysa");
            VisitorTravelLog::Abandoned(0x00001234u, "no acceptable point on the chain");
            VisitorTravelLog::OnSessionEnd();

            REQUIRE(AnyLineContains("abandoned"));
            REQUIRE(AnyLineContains("no acceptable point on the chain"));
        }
    }

    SECTION("when the session has ended")
    {
        VisitorTravelLog::OnSessionStart();
        VisitorTravelLog::OnSessionEnd();

        SECTION("should stop reporting itself active")
        {
            REQUIRE_FALSE(VisitorTravelLog::IsActive());
        }

        SECTION("should drop anything written afterwards rather than crash")
        {
            // The stream is closed between sessions, and emitters are called
            // from two threads; a line arriving late is a no-op.
            VisitorTravelLog::Write("GATE", "written after the file closed");
            REQUIRE_FALSE(AnyLineContains("written after the file closed"));
        }

        SECTION("should say how much it wrote")
        {
            REQUIRE(AnyLineContains("closed after"));
        }
    }

    SECTION("when the setting is off")
    {
        const ConfiguredSettings disabled{kOff};
        VisitorTravelLog::OnSessionStart();

        SECTION("should not open a file at all")
        {
            // Off means off: no file, and no cost per line beyond one
            // atomic read.
            VisitorTravelLog::Begin(0x0001C1A9u, "Faralda");
            VisitorTravelLog::Write("GATE", "nothing should land here");
            REQUIRE_FALSE(VisitorTravelLog::IsActive());
            REQUIRE_FALSE(std::filesystem::exists(TracePath()));
        }
    }

    SECTION("when a second session starts")
    {
        VisitorTravelLog::OnSessionStart();
        VisitorTravelLog::Write("GATE", "the first session");
        VisitorTravelLog::OnSessionEnd();
        VisitorTravelLog::OnSessionStart();
        VisitorTravelLog::Write("GATE", "the second session");

        SECTION("should keep the previous one beside it")
        {
            // Rotation is why a tester can start a fresh run and still attach
            // the one that showed the bug.
            VisitorTravelLog::OnSessionEnd();
            REQUIRE(AnyLineContains("the second session", 0));
            REQUIRE(AnyLineContains("the first session", 1));
        }
    }

    ClearTrace();
}
