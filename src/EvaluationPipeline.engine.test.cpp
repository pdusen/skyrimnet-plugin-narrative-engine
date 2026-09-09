#include <EvaluationPipeline.h>

#include <ConfiguredSettings.h>
#include <DecisionLog.h>
#include <DownstreamSpies.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <PhaseTracker.h>
#include <PluginThread.h>
#include <SkyrimNetAPI.h>
#include <Snapshot.h>
#include <ThreadRole.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstddef>
#include <string>

// Tests for the Director's per-tick evaluation.
//
// This is the loop the whole mod turns on: read the world, ask an LLM what is
// happening in it, and hand the answer to the beat system. Everything about it
// that can go wrong goes wrong quietly.
//
// The single-flight latch is the sharpest example. An LLM round trip routinely
// outlasts several tick intervals, so without a latch each interval starts
// another evaluation and the first answer to return unleashes the backlog. With
// a latch that is never released — a throw on the way to the LLM, an exception
// out of the beat system — the Director stops for the rest of the session and
// says nothing at all. Both failures look exactly like "the mod is quiet".
//
// The parse is the other half. An LLM's answer is untrusted text: it arrives
// wrapped in a markdown fence as often as not, sometimes is not JSON, and the
// fields inside it are whatever the model felt like. So the record is pre-filled
// from the snapshot before parsing begins, and a response that gives nothing
// still produces a usable decision rather than a hole in the log.
//
// The LLM here is the stand-in SkyrimNet beside the executable, whose answer a
// test sets directly. The beat system and the dashboard are stand-ins too — the
// evaluation's contract with them is a handoff, and what a test wants to know is
// what was handed over.

namespace
{
    namespace EvaluationPipeline = NarrativeEngine::EvaluationPipeline;
    namespace DecisionLog = NarrativeEngine::DecisionLog;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace SkyrimNetAPI = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Snapshot;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::DownstreamSpies;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    constexpr const char* kSettings = "[General]\nbDebugMode=0\n";

    FakeSkyrimNetState& FakeLLM()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    template <std::size_t N> void Say(char (&dest)[N], const std::string& text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && i < text.size(); ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    // Runs one evaluation the way the tick driver does, on a plugin thread.
    void Evaluate()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [](const PluginThread::Token& pt) { EvaluationPipeline::BeginEvaluation(pt); });
    }

    void ApplyOnPluginThread(const DecisionLog::DecisionRecord& record)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { EvaluationPipeline::ApplyDecision(pt, record); });
    }

    // A snapshot with the fields the parse pre-fills from, so a case can tell
    // a value that survived from one that was invented.
    Snapshot SnapshotInPhase(PhaseTracker::Phase phase)
    {
        Snapshot s;
        s.realTimeSec = 1234.0;
        s.currentPhase = PhaseTracker::PhaseName(phase);
        s.player.gameDaysPassed = 42.0f;
        s.alphaCanonSignalBitmask = 0x000000A5u;
        return s;
    }
} // namespace

TEST_CASE("EvaluationPipeline unwraps what the model actually sent", "[EvaluationPipeline][engine]")
{
    // Models wrap JSON in a markdown fence however firmly they are told not to,
    // and a wrapped answer that is thrown away is a tick where the Director
    // learned nothing.

    SECTION("when the answer is bare JSON")
    {
        SECTION("should leave it alone")
        {
            REQUIRE(EvaluationPipeline::StripMarkdownFences("{\"a\":1}") == "{\"a\":1}");
        }
    }

    SECTION("when the answer is wrapped in a tagged fence")
    {
        SECTION("should return what was inside")
        {
            REQUIRE(EvaluationPipeline::StripMarkdownFences("```json\n{\"a\":1}\n```") == "{\"a\":1}");
        }
    }

    SECTION("when the fence carries no language tag")
    {
        SECTION("should return what was inside")
        {
            REQUIRE(EvaluationPipeline::StripMarkdownFences("```\n{\"a\":1}\n```") == "{\"a\":1}");
        }
    }

    SECTION("when the answer is padded with whitespace")
    {
        SECTION("should trim it")
        {
            REQUIRE(EvaluationPipeline::StripMarkdownFences("  \n\t{\"a\":1}\r\n  ") == "{\"a\":1}");
        }
    }

    SECTION("when the answer is only whitespace")
    {
        SECTION("should return nothing")
        {
            REQUIRE(EvaluationPipeline::StripMarkdownFences("   \n\t ").empty());
        }
    }

    SECTION("when a fence was opened and never closed")
    {
        SECTION("should still return what followed it")
        {
            // Truncation at the token limit is the ordinary cause, and the
            // body up to that point is usually still parseable.
            REQUIRE(EvaluationPipeline::StripMarkdownFences("```json\n{\"a\":1}") == "{\"a\":1}");
        }
    }

    SECTION("when a fence was opened on the same line as the body")
    {
        SECTION("should leave the answer alone")
        {
            // With no newline there is no way to tell a language tag from the
            // start of the body, and guessing would eat the first field.
            REQUIRE(EvaluationPipeline::StripMarkdownFences("```{\"a\":1}```") == "```{\"a\":1}```");
        }
    }
}

TEST_CASE("EvaluationPipeline reads what the model said", "[EvaluationPipeline][engine]")
{
    // Happy path, re-run per leaf: a snapshot in the opening phase, and an
    // answer that is well-formed JSON.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const auto snapshot = SnapshotInPhase(PhaseTracker::Phase::Exposition);

    SECTION("when the answer is well formed")
    {
        const auto record = EvaluationPipeline::ParseDecision(
            "{\"tension_score\": 55, \"narrative_note\": \"the town is uneasy\"}", snapshot);

        SECTION("should take the tension it reported")
        {
            REQUIRE(record.tensionScore == 55);
        }

        SECTION("should take the note it wrote")
        {
            REQUIRE(record.narrativeNote == "the town is uneasy");
        }

        SECTION("should carry the snapshot's own facts across")
        {
            // The record is what the dashboard and the next tick read, so it
            // has to say when it was taken and what the world looked like even
            // though the model was never asked about any of that.
            REQUIRE(record.realTimeSec == snapshot.realTimeSec);
            REQUIRE(record.gameDaysPassed == snapshot.player.gameDaysPassed);
            REQUIRE(record.alphaCanonActiveSignals == snapshot.alphaCanonSignalBitmask);
            REQUIRE(record.currentPhase == PhaseTracker::Phase::Exposition);
        }
    }

    SECTION("when the answer is wrapped in a fence")
    {
        SECTION("should read it anyway")
        {
            const auto record = EvaluationPipeline::ParseDecision("```json\n{\"tension_score\": 12}\n```", snapshot);
            REQUIRE(record.tensionScore == 12);
        }
    }

    SECTION("when the answer is not JSON at all")
    {
        const auto record = EvaluationPipeline::ParseDecision("I think things are quite tense right now.", snapshot);

        SECTION("should say so in the record")
        {
            // The record still reaches the log and the dashboard, so a failure
            // that left the note empty would be indistinguishable from a model
            // that had nothing to say.
            REQUIRE(record.narrativeNote.starts_with("parse_failure"));
        }

        SECTION("should still be usable")
        {
            REQUIRE(record.realTimeSec == snapshot.realTimeSec);
            REQUIRE(record.currentPhase == PhaseTracker::Phase::Exposition);
        }
    }

    SECTION("when the answer is JSON but not an object")
    {
        SECTION("should say so in the record")
        {
            const auto record = EvaluationPipeline::ParseDecision("[1, 2, 3]", snapshot);
            REQUIRE(record.narrativeNote.starts_with("parse_failure"));
        }
    }

    SECTION("when the tension is outside the range asked for")
    {
        SECTION("should pull it back inside")
        {
            // The score gates phase advancement, and a model that answered 900
            // would advance the story on the strength of a typo.
            REQUIRE(EvaluationPipeline::ParseDecision("{\"tension_score\": 900}", snapshot).tensionScore == 100);
            REQUIRE(EvaluationPipeline::ParseDecision("{\"tension_score\": -40}", snapshot).tensionScore == 0);
        }
    }

    SECTION("when the tension is not a number")
    {
        SECTION("should leave it at nothing")
        {
            // Rather than coercing: a string that happened to begin with a
            // digit would otherwise advance the phase.
            REQUIRE(EvaluationPipeline::ParseDecision("{\"tension_score\": \"high\"}", snapshot).tensionScore == 0);
        }
    }

    SECTION("when the note is longer than the record keeps")
    {
        SECTION("should cut it down")
        {
            const std::string wall(500, 'x');
            const auto record = EvaluationPipeline::ParseDecision("{\"narrative_note\": \"" + wall + "\"}", snapshot);
            REQUIRE(record.narrativeNote.size() <= 200);
        }
    }

    SECTION("when the note carries characters the game cannot draw")
    {
        SECTION("should replace them")
        {
            // Every string an LLM hands back goes through the sanitizer before
            // it is stored: smart quotes and dashes are missing glyphs in
            // Skyrim, and the co-save carries this note.
            const auto record =
                EvaluationPipeline::ParseDecision("{\"narrative_note\": \"the \\u201ctown\\u201d\"}", snapshot);
            REQUIRE(record.narrativeNote == "the \"town\"");
        }
    }
}

TEST_CASE("EvaluationPipeline hands its decision on", "[EvaluationPipeline][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    DownstreamSpies().Reset();
    DecisionLog::Clear();
    PhaseTracker::Reset();

    DecisionLog::DecisionRecord record;
    record.realTimeSec = 99.0;
    record.tensionScore = 40;
    record.narrativeNote = "something happened";

    SECTION("when a decision is applied")
    {
        ApplyOnPluginThread(record);

        SECTION("should put it in the log")
        {
            // Before anything else, because the next tick's snapshot reads the
            // log — a decision applied after the push would be invisible to
            // the evaluation that follows it.
            const auto tail = DecisionLog::Tail(1);
            REQUIRE(tail.size() == 1);
            REQUIRE(tail.front().narrativeNote == "something happened");
        }

        SECTION("should tell the dashboard the world moved")
        {
            REQUIRE(DownstreamSpies().dashboardPushes.load() > 0);
        }

        SECTION("should leave the phase where it was")
        {
            REQUIRE(PhaseTracker::Get() == PhaseTracker::Phase::Exposition);
        }
    }

    SECTION("when the decision advances the phase")
    {
        record.advancedToPhase = PhaseTracker::Phase::RisingAction;
        ApplyOnPluginThread(record);

        SECTION("should move the story on")
        {
            // The phase is what every other subsystem reads to know what kind
            // of story it is in, so this is the one side effect an evaluation
            // has on the rest of the mod.
            REQUIRE(PhaseTracker::Get() == PhaseTracker::Phase::RisingAction);
        }
    }
}

TEST_CASE("EvaluationPipeline runs one evaluation at a time", "[EvaluationPipeline][engine]")
{
    // The single-flight latch, which is what stands between a slow LLM and a
    // burst of queued evaluations arriving at once.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    DownstreamSpies().Reset();
    REQUIRE(SkyrimNetAPI::Initialize());
    auto& llm = FakeLLM();
    llm.sendPromptAccepts = true;
    llm.sendPromptSucceeds = true;
    Say(llm.promptResponse, "{\"tension_score\": 30}");

    SECTION("when nothing is running")
    {
        SECTION("should report itself idle")
        {
            REQUIRE_FALSE(EvaluationPipeline::IsEvaluationInFlight());
        }

        SECTION("should hand a decision to the beat system")
        {
            Evaluate();
            REQUIRE(DownstreamSpies().beatsConsidered.load() == 1);
        }

        SECTION("should be idle again once the beat system is finished")
        {
            Evaluate();
            REQUIRE_FALSE(EvaluationPipeline::IsEvaluationInFlight());
        }
    }

    SECTION("when the beat system is still holding the last decision")
    {
        // Which is what an LLM round trip outlasting the tick interval looks
        // like from here.
        DownstreamSpies().holdCompletion = true;
        Evaluate();
        REQUIRE(DownstreamSpies().beatsConsidered.load() == 1);

        SECTION("should report itself busy")
        {
            REQUIRE(EvaluationPipeline::IsEvaluationInFlight());
        }

        SECTION("should refuse to start another")
        {
            // Not queue it. One evaluation per missed interval would arrive as
            // a burst the moment the first returned, and every one of them
            // would be reasoning about a world that had already moved.
            Evaluate();
            Evaluate();
            REQUIRE(DownstreamSpies().beatsConsidered.load() == 1);
        }

        SECTION("should start another once it is released")
        {
            DownstreamSpies().holdCompletion = false;
            DownstreamSpies().ReleaseHeld();
            REQUIRE_FALSE(EvaluationPipeline::IsEvaluationInFlight());
            Evaluate();
            REQUIRE(DownstreamSpies().beatsConsidered.load() == 2);
        }
    }

    SECTION("when the model call fails")
    {
        llm.sendPromptSucceeds = false;
        Evaluate();

        SECTION("should hand nothing on")
        {
            // A failed call is not a decision. Passing the empty response
            // through would file a parse failure as though the model had
            // answered.
            REQUIRE(DownstreamSpies().beatsConsidered.load() == 0);
        }

        SECTION("should not stay latched")
        {
            // The failure that silently kills the Director for the rest of the
            // session: a latch taken and never released means every later tick
            // skips, and nothing in the log says why.
            REQUIRE_FALSE(EvaluationPipeline::IsEvaluationInFlight());
        }
    }

    SECTION("when the model cannot be reached at all")
    {
        llm.sendPromptAccepts = false;
        Evaluate();

        SECTION("should not stay latched")
        {
            REQUIRE_FALSE(EvaluationPipeline::IsEvaluationInFlight());
        }
    }
}

TEST_CASE("EvaluationPipeline asks about the world it snapshotted", "[EvaluationPipeline][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    PhaseTracker::Reset();

    SECTION("when a snapshot is taken")
    {
        const auto snapshot = EvaluationPipeline::BuildSnapshot();

        SECTION("should name the phase the story is in")
        {
            REQUIRE(snapshot.currentPhase == PhaseTracker::PhaseName(PhaseTracker::Get()));
        }

        SECTION("should carry the world's clock")
        {
            REQUIRE(snapshot.player.gameDaysPassed == engine.calendar.daysPassed);
        }
    }

    SECTION("when the prompt context is built from it")
    {
        auto snapshot = SnapshotInPhase(PhaseTracker::Phase::RisingAction);
        snapshot.timeInPhaseSeconds = 300.0f;
        snapshot.skyrimNetEventsJSON = "[]";
        const auto context = nlohmann::json::parse(EvaluationPipeline::BuildPromptContext(snapshot));

        SECTION("should tell the model what phase it is in")
        {
            // The one thing the model cannot work out for itself, and the
            // thing every threshold it is scored against depends on.
            REQUIRE(context.value("current_phase", std::string{}) == snapshot.currentPhase);
        }

        SECTION("should tell it how long that has been true")
        {
            REQUIRE(context.value("time_in_phase_seconds", 0.0) == 300.0);
        }
    }

    SECTION("when the events are from before this phase began")
    {
        auto snapshot = SnapshotInPhase(PhaseTracker::Phase::RisingAction);
        snapshot.phaseEnteredAtRealTime = 1000.0;
        snapshot.skyrimNetEventsJSON = "[{\"type\":\"dialogue\",\"localTime\":2000.0,\"text\":\"now\"},"
                                       "{\"type\":\"dialogue\",\"localTime\":500.0,\"text\":\"before\"}]";
        const auto context = nlohmann::json::parse(EvaluationPipeline::BuildPromptContext(snapshot));

        SECTION("should leave them out")
        {
            // Past phases have already been answered. Letting their events
            // back in re-justifies the same advance every tick, and the story
            // runs away from the player. Identified by their timestamp, not by
            // their words: the renderer rewrites the text of every event it
            // passes on.
            const auto& events = context.at("recent_events");
            REQUIRE(events.size() == 1);
            REQUIRE(events[0].value("localTime", 0.0) == 2000.0);
        }
    }
}
