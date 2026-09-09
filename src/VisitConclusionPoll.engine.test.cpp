#include <VisitConclusionPoll.h>

#include <AsyncDispatch.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <SkyrimNetAPI.h>
#include <VisitState.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

// Tests for deciding when a visit has run its course.
//
// An NPC has walked across the province to say something and is now standing in
// front of the player talking. Somebody has to notice when that conversation is
// over, and getting it wrong is bad in both directions: end it early and the
// visitor turns and leaves mid-sentence; never end it and they stand there
// permanently, which is the failure players actually report.
//
// The judgement itself is a model call and it is expensive, so it is gated
// behind three cheap thresholds checked once a second. Any one of them is
// enough — enough has been said since the last poll, nobody has spoken for a
// while, or too long has simply gone by. The gate is where nearly all the value
// is: a gate that never trips is a visitor who never leaves, and a gate that
// trips constantly is an LLM call every second for the length of a
// conversation.
//
// The silence threshold is measured in REAL seconds with menus, dialogue views
// and combat subtracted, not in game time. Game time made the whole cadence
// depend on the player's timescale setting, and on a slow one the gate tripped
// while the player was simply reading. What is wanted is "the player was in the
// world, doing nothing" — which is when a human conversation partner would
// assume they had been forgotten.

namespace
{
    namespace Poll = NarrativeEngine::VisitConclusionPoll;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    namespace VisitState = NarrativeEngine::VisitState;
    using NarrativeEngine::TickMode;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    // A turn threshold of two, silence off, and no interval — so the cases
    // about one threshold are never answered by another.
    constexpr const char* kTurnsOnly = "[Beats]\niVisitPollTurnCountThreshold=2\n"
                                       "iVisitPollSilenceRealSeconds=0\niVisitPollMaxIntervalGameMinutes=0\n";

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

    VisitState::Snapshot SnapshotFor(std::uint32_t sender = kYsolda)
    {
        VisitState::Snapshot s;
        s.senderFormID = sender;
        s.briefingText = "I have been meaning to say this to your face.";
        s.narrationText = "She crosses the market and stops in front of you.";
        s.topicTag = "the tusk";
        s.mood = "warm";
        return s;
    }

    std::string Verdict(bool conclude, const char* rationale = "they have said their piece")
    {
        nlohmann::json j;
        j["should_conclude"] = conclude;
        j["rationale"] = rationale;
        return j.dump();
    }

    // The poll's callback is delivered on a worker, so the call is made and
    // then waited on. Bounded and asserted, so a poll that stops answering
    // fails the run rather than hanging it.
    std::optional<Poll::PollVerdict> Fire(bool& answered)
    {
        std::optional<Poll::PollVerdict> result;
        answered = false;
        Poll::FirePoll([&](std::optional<Poll::PollVerdict> verdict) {
            result = std::move(verdict);
            answered = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!answered && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return result;
    }

    struct RunningDispatch
    {
        RunningDispatch()
        {
            AsyncDispatch::Start();
        }

        ~RunningDispatch()
        {
            AsyncDispatch::Stop();
        }

        RunningDispatch(const RunningDispatch&) = delete;
        RunningDispatch& operator=(const RunningDispatch&) = delete;
    };
} // namespace

TEST_CASE("VisitConclusionPoll arms for one conversation at a time", "[VisitConclusionPoll][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kTurnsOnly};
    // A world clock that has run for a while. The poll stamps game-time
    // readings and treats zero as "never", so a save at the very first
    // instant of a new game would be indistinguishable from no visit at all.
    engine.calendar.hoursPassed = 1008.0f;
    Poll::Disarm();

    SECTION("when nothing is being discussed")
    {
        SECTION("should be disarmed")
        {
            REQUIRE_FALSE(Poll::IsArmed());
        }

        SECTION("should never trip its gate")
        {
            // The gate runs once a second for the whole session, not just
            // during a visit. Tripping outside one would fire a model call
            // about a conversation nobody is having.
            Poll::RegisterSpeechTurn();
            Poll::RegisterSpeechTurn();
            Poll::RegisterSpeechTurn();
            REQUIRE_FALSE(Poll::GateTick());
        }

        SECTION("should report no silence")
        {
            REQUIRE(Poll::SilenceRealSeconds() == 0.0);
        }

        SECTION("should report no start time")
        {
            REQUIRE(Poll::DiscussStartedAtGameSeconds() == 0.0);
        }
    }

    SECTION("when a conversation begins")
    {
        Poll::Arm(SnapshotFor());

        SECTION("should be armed")
        {
            REQUIRE(Poll::IsArmed());
        }

        SECTION("should remember when it began")
        {
            // Consumers filter the dialogue they sample by this, or old lines
            // from an earlier conversation with the same NPC poison the poll
            // and it reads the visit as a continuation of something else.
            REQUIRE(Poll::DiscussStartedAtGameSeconds() > 0.0);
        }
    }

    SECTION("when the conversation ends")
    {
        Poll::Arm(SnapshotFor());
        Poll::Disarm();

        SECTION("should be disarmed again")
        {
            REQUIRE_FALSE(Poll::IsArmed());
        }

        SECTION("should be safe to end twice")
        {
            // Disarm is reached from every exit — the goodbye, a rollback,
            // and a hard abort — and more than one of them can happen.
            Poll::Disarm();
            REQUIRE_FALSE(Poll::IsArmed());
        }
    }
}

TEST_CASE("VisitConclusionPoll waits for enough to have been said", "[VisitConclusionPoll][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kTurnsOnly};
    engine.calendar.hoursPassed = 1008.0f;
    Poll::Disarm();
    Poll::Arm(SnapshotFor());

    SECTION("when barely anything has been said")
    {
        Poll::RegisterSpeechTurn();

        SECTION("should hold off")
        {
            // One line in is far too early to ask whether a conversation is
            // over, and asking costs a model call.
            REQUIRE_FALSE(Poll::GateTick());
        }
    }

    SECTION("when the exchange has run its threshold of turns")
    {
        Poll::RegisterSpeechTurn();
        Poll::RegisterSpeechTurn();

        SECTION("should trip")
        {
            REQUIRE(Poll::GateTick());
        }
    }

    SECTION("when the turn threshold is switched off")
    {
        const ConfiguredSettings quiet{"[Beats]\niVisitPollTurnCountThreshold=0\n"
                                       "iVisitPollSilenceRealSeconds=0\niVisitPollMaxIntervalGameMinutes=0\n"};
        Poll::RegisterSpeechTurn();
        Poll::RegisterSpeechTurn();
        Poll::RegisterSpeechTurn();

        SECTION("should never trip on turns")
        {
            // Zero means off rather than "trip immediately", which is what a
            // threshold compared with >= would do to it.
            REQUIRE_FALSE(Poll::GateTick());
        }
    }
}

TEST_CASE("VisitConclusionPoll notices a silence", "[VisitConclusionPoll][engine]")
{
    // Silence is counted in real seconds with menus, dialogue views and combat
    // subtracted — the accumulator only advances on an ordinary tick.
    EngineMock engine;
    const ConfiguredSettings settings{"[Beats]\niVisitPollTurnCountThreshold=0\n"
                                      "iVisitPollSilenceRealSeconds=1\niVisitPollMaxIntervalGameMinutes=0\n"};
    Poll::Disarm();
    Poll::Arm(SnapshotFor());
    Poll::RegisterSpeechTurn();

    SECTION("when the player is standing in the world doing nothing")
    {
        Poll::TickAccumulator(TickMode::Normal);
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        Poll::TickAccumulator(TickMode::Normal);

        SECTION("should count the time towards a silence")
        {
            REQUIRE(Poll::SilenceRealSeconds() >= 1.0);
        }

        SECTION("should trip once the silence is long enough")
        {
            REQUIRE(Poll::GateTick());
        }
    }

    SECTION("when the player is in a menu or a dialogue view")
    {
        Poll::TickAccumulator(TickMode::Normal);
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        Poll::TickAccumulator(TickMode::Paused);

        SECTION("should not count that time at all")
        {
            // Reading a dialogue view is not being ignored. Counting it made
            // the visitor give up while the player was still reading what
            // they had just said.
            REQUIRE(Poll::SilenceRealSeconds() == 0.0);
            REQUIRE_FALSE(Poll::GateTick());
        }
    }

    SECTION("when the game resumes after a long pause")
    {
        Poll::TickAccumulator(TickMode::Normal);
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        Poll::TickAccumulator(TickMode::Paused);
        Poll::TickAccumulator(TickMode::Normal);

        SECTION("should not credit the whole pause to the silence")
        {
            // The baseline advances on every tick whatever the mode, which is
            // the only thing keeping a resumed tick from charging the entire
            // menu session to the silence in one go.
            REQUIRE(Poll::SilenceRealSeconds() < 1.0);
        }
    }

    SECTION("when somebody speaks")
    {
        Poll::TickAccumulator(TickMode::Normal);
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        Poll::TickAccumulator(TickMode::Normal);
        REQUIRE(Poll::SilenceRealSeconds() >= 1.0);
        Poll::RegisterSpeechTurn();

        SECTION("should start the silence over")
        {
            REQUIRE(Poll::SilenceRealSeconds() == 0.0);
        }
    }
}

TEST_CASE("VisitConclusionPoll asks the model whether it is over", "[VisitConclusionPoll][engine]")
{
    // Happy path, re-run per leaf: an armed conversation and a model that
    // answers with a verdict.
    EngineMock engine;
    const ConfiguredSettings settings{kTurnsOnly};
    const RunningDispatch dispatch;
    auto& fake = FakeState();
    REQUIRE(SkyrimNet::Initialize());
    fake.Reset();
    engine.calendar.hoursPassed = 1008.0f;
    (void)engine.AddActor(kYsolda);
    SetJson(fake.promptResponse, Verdict(true));
    Poll::Disarm();
    Poll::Arm(SnapshotFor());
    bool answered = false;

    SECTION("when the model says the visit is over")
    {
        const auto verdict = Fire(answered);

        SECTION("should say so")
        {
            REQUIRE(answered);
            REQUIRE(verdict.has_value());
            REQUIRE(verdict->shouldConclude);
        }

        SECTION("should carry the reason it gave")
        {
            // The rationale reaches the dashboard, which is the only place
            // anyone can see why a visit ended when it did.
            REQUIRE(verdict.has_value());
            REQUIRE(verdict->rationale == "they have said their piece");
        }

        SECTION("should assume the goodbye has not been said")
        {
            // The flag is optional in the answer, and its absence has to mean
            // the safe thing: a narrated goodbye where none was spoken reads
            // oddly, but suppressing one that was never said leaves the
            // visitor walking off in silence.
            REQUIRE(verdict.has_value());
            REQUIRE_FALSE(verdict->closingAlreadySpoken);
        }
    }

    SECTION("when the model says the goodbye has already been spoken")
    {
        nlohmann::json j;
        j["should_conclude"] = true;
        j["rationale"] = "she has said goodbye";
        j["closing_already_spoken"] = true;
        SetJson(fake.promptResponse, j.dump());
        const auto verdict = Fire(answered);

        SECTION("should pass that on")
        {
            // What it decides downstream is whether the closing narration is
            // spoken aloud or filed silently — the difference between a
            // natural exit and the visitor saying goodbye twice.
            REQUIRE(verdict.has_value());
            REQUIRE(verdict->closingAlreadySpoken);
        }
    }

    SECTION("when the model says there is more to come")
    {
        SetJson(fake.promptResponse, Verdict(false, "she has not got to the point"));
        const auto verdict = Fire(answered);

        SECTION("should say so")
        {
            REQUIRE(verdict.has_value());
            REQUIRE_FALSE(verdict->shouldConclude);
        }
    }

    SECTION("when the poll fires after the conversation ended")
    {
        Poll::Disarm();
        const auto verdict = Fire(answered);

        SECTION("should answer with nothing")
        {
            // The visit is already unwinding, and a verdict arriving now would
            // act on a conversation that has been taken down.
            REQUIRE(answered);
            REQUIRE_FALSE(verdict.has_value());
        }
    }

    SECTION("when the model call fails")
    {
        fake.sendPromptSucceeds = false;
        const auto verdict = Fire(answered);

        SECTION("should answer with nothing")
        {
            REQUIRE(answered);
            REQUIRE_FALSE(verdict.has_value());
        }

        SECTION("should count the failure")
        {
            // Enough of these in a row and the beat gives up on the visit
            // rather than leaving the visitor standing there for good.
            REQUIRE(Poll::ConsecutivePollFailures() > 0);
        }
    }

    SECTION("when the answer is not JSON")
    {
        SetJson(fake.promptResponse, "I think she is finished.");

        SECTION("should answer with nothing")
        {
            const auto verdict = Fire(answered);
            REQUIRE(answered);
            REQUIRE_FALSE(verdict.has_value());
        }
    }

    SECTION("when the answer carries no verdict")
    {
        SetJson(fake.promptResponse, R"({"rationale":"hard to say"})");

        SECTION("should answer with nothing")
        {
            // Rather than reading a missing field as "no". A visit that never
            // concludes is the failure players report, and inventing a "no"
            // is how it happens.
            const auto verdict = Fire(answered);
            REQUIRE(answered);
            REQUIRE_FALSE(verdict.has_value());
        }
    }

    SECTION("when a good answer follows a failed one")
    {
        fake.sendPromptSucceeds = false;
        (void)Fire(answered);
        REQUIRE(Poll::ConsecutivePollFailures() > 0);
        fake.sendPromptSucceeds = true;
        (void)Fire(answered);

        SECTION("should forget the failures")
        {
            // The count is for consecutive failures — a single bad round trip
            // in a long conversation must not accumulate towards an abort.
            REQUIRE(Poll::ConsecutivePollFailures() == 0);
        }
    }

    SECTION("when a poll has just fired")
    {
        Poll::RegisterSpeechTurn();
        Poll::RegisterSpeechTurn();
        REQUIRE(Poll::GateTick());
        (void)Fire(answered);

        SECTION("should make the gate start over")
        {
            // Whatever the verdict. Without the reset the gate stays tripped
            // and the poll fires every tick for the rest of the visit.
            REQUIRE_FALSE(Poll::GateTick());
        }
    }

    SECTION("when there is nobody to hand the answer to")
    {
        SECTION("should do nothing at all")
        {
            const int before = fake.sendPromptCalls;
            Poll::FirePoll(nullptr);
            REQUIRE(fake.sendPromptCalls == before);
        }
    }
}
