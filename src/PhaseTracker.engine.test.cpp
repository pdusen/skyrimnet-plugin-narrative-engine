#include <PhaseTracker.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <EventLogSpies.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Mocked-engine tests for the Freytag-pyramid phase tracker.
//
// The tracker is the Director's only long-lived narrative state: which phase
// the story is in, how long it has been there, and when it got there. Each of
// those is read by something that decides whether to act, and none of them is
// visible in game until a beat fires or fails to. The persistence half carries
// the sharpest risk — three record versions share one loader, and v2's trailing
// field means something different from v3's while occupying the same eight
// bytes.
//
// Two things had to be stood in for. The module notifies three event logs on
// every advance; two of those are still stand-ins, in
// testsupport/EventLogSpies.cpp, and they are spies as much as stand-ins —
// "did the advance tell the logs" is real behaviour and is asserted below. The
// third, WeatherEventLog, is compiled in for real and is not counted here; its
// own tests cover what an advance does to it. The other stand-in is the co-save
// interface, which EngineMock already backs with a byte-accurate stream.
//
// The tracker's state is process-wide, so every TEST_CASE that touches it
// begins by resetting. Reset is public and is the same call the plugin makes on
// revert.

namespace
{
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using PhaseTracker::Direction;
    using PhaseTracker::Phase;

    // A Unix-epoch anchor far enough in the past that it cannot be confused
    // with one the tracker just captured, and a floor that any freshly captured
    // anchor clears (2001-09-09).
    constexpr double kOldAnchor = 1'700'000'000.0;
    constexpr double kRecentEpochSeconds = 1'000'000'000.0;

    // Long enough that the difference between "the clock ran" and "the clock
    // was frozen" is far outside timer noise, short enough not to slow the
    // suite down noticeably.
    constexpr int kSleepMilliseconds = 40;
    constexpr float kHalfSleepSeconds = 0.020f;

    // A non-null interface pointer; the mocked SKSE methods answer out of
    // EngineMock and never read through it.
    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    // Puts the process-wide tracker back to a known state and zeroes the
    // notification counters. Declared as a local in each TEST_CASE so it re-runs
    // for every leaf path — RAII in place of a reset hook.
    struct FreshTracker
    {
        FreshTracker()
        {
            PhaseTracker::Reset();
            NarrativeEngine::Testing::EventLogSpies().Reset();
        }

        FreshTracker(const FreshTracker&) = delete;
        FreshTracker& operator=(const FreshTracker&) = delete;
    };

    // The tracker's clock only advances on Tick, which needs a plugin token.
    void TickOnPluginThread()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([](const PluginThread::Token& pt) { PhaseTracker::Tick(pt); });
    }

    // Let enough wall-clock pass for the dwell accumulator to have something to
    // accumulate. The tracker samples a steady clock, so real time is the only
    // way to move it.
    void Elapse()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMilliseconds));
    }

    template <class T> void Append(std::vector<std::byte>& out, const T& value)
    {
        const auto* first = reinterpret_cast<const std::byte*>(&value);
        out.insert(out.end(), first, first + sizeof(T));
    }

    // Payloads built by hand rather than by OnSave, so the loader is fed the
    // bytes a past build would have written rather than whatever this one
    // produces. v1 has no trailing field; v2 and v3 share a layout and differ
    // only in what the trailing eight bytes mean.
    std::vector<std::byte> LoadedPayloadV1(std::uint8_t phaseByte, float dwellSeconds)
    {
        std::vector<std::byte> payload;
        Append(payload, phaseByte);
        Append(payload, dwellSeconds);
        return payload;
    }

    std::vector<std::byte> LoadedPayload(std::uint8_t phaseByte, float dwellSeconds, double anchor)
    {
        auto payload = LoadedPayloadV1(phaseByte, dwellSeconds);
        Append(payload, anchor);
        return payload;
    }
} // namespace

TEST_CASE("PhaseTracker::PhaseName", "[PhaseTracker][engine]")
{
    SECTION("when the phase is a real one")
    {
        SECTION("should name it")
        {
            // These strings are the wire format for the ne_narrative_phase
            // decorator and for PhaseFromName, so they are not free to change.
            REQUIRE(std::string{PhaseTracker::PhaseName(Phase::Exposition)} == "Exposition");
            REQUIRE(std::string{PhaseTracker::PhaseName(Phase::RisingAction)} == "RisingAction");
            REQUIRE(std::string{PhaseTracker::PhaseName(Phase::Climax)} == "Climax");
            REQUIRE(std::string{PhaseTracker::PhaseName(Phase::FallingAction)} == "FallingAction");
            REQUIRE(std::string{PhaseTracker::PhaseName(Phase::Resolution)} == "Resolution");
        }
    }

    SECTION("when the phase is out of range")
    {
        SECTION("should say unknown")
        {
            // Count is the sentinel and is one past the last name, so it is
            // the boundary the guard exists for.
            REQUIRE(std::string{PhaseTracker::PhaseName(Phase::Count)} == "Unknown");
            REQUIRE(std::string{PhaseTracker::PhaseName(static_cast<Phase>(99))} == "Unknown");
        }
    }
}

TEST_CASE("PhaseTracker::PhaseFromName", "[PhaseTracker][engine]")
{
    SECTION("when the name is one the tracker emits")
    {
        SECTION("should round-trip every phase")
        {
            // The pair is what matters: a saved or MCM-supplied phase name has
            // to come back as the phase that produced it.
            for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Phase::Count); ++i) {
                const auto phase = static_cast<Phase>(i);
                REQUIRE(PhaseTracker::PhaseFromName(PhaseTracker::PhaseName(phase)) == phase);
            }
        }
    }

    SECTION("when the name is not a phase")
    {
        SECTION("should return nothing")
        {
            REQUIRE_FALSE(PhaseTracker::PhaseFromName("Denouement").has_value());
            REQUIRE_FALSE(PhaseTracker::PhaseFromName("").has_value());
            // Case matters; the names are identifiers, not prose.
            REQUIRE_FALSE(PhaseTracker::PhaseFromName("climax").has_value());
        }
    }
}

TEST_CASE("PhaseTracker::NextPhase", "[PhaseTracker][engine]")
{
    SECTION("when the phase has a successor")
    {
        SECTION("should return it")
        {
            REQUIRE(PhaseTracker::NextPhase(Phase::Exposition) == Phase::RisingAction);
            REQUIRE(PhaseTracker::NextPhase(Phase::RisingAction) == Phase::Climax);
            REQUIRE(PhaseTracker::NextPhase(Phase::Climax) == Phase::FallingAction);
            REQUIRE(PhaseTracker::NextPhase(Phase::FallingAction) == Phase::Resolution);
        }
    }

    SECTION("when the phase is the last in the cycle")
    {
        SECTION("should wrap to the start")
        {
            // The cycle is deliberately endless: a story that reached
            // Resolution starts a new arc rather than the Director going quiet
            // for the rest of the save.
            REQUIRE(PhaseTracker::NextPhase(Phase::Resolution) == Phase::Exposition);
        }
    }

    SECTION("when the phase is out of range")
    {
        SECTION("should fall back to the start")
        {
            REQUIRE(PhaseTracker::NextPhase(static_cast<Phase>(99)) == Phase::Exposition);
        }
    }
}

TEST_CASE("PhaseTracker::EvaluateAdvance dwell floor", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: a 30-second floor and an Exposition
    // threshold of 45, so a score of 90 is unambiguously over the line and the
    // only thing that can hold the advance back is the dwell.
    EngineMock engine;
    const ConfiguredSettings settings{"[Director]\niMinPhaseDurationSeconds=30\niAdvanceThresholdExposition=45\n"};

    SECTION("when the phase has not been held long enough")
    {
        SECTION("should refuse however high the tension")
        {
            // The floor separates "advancement is permitted" from "a beat
            // would like to nudge things along". Without it a single dramatic
            // tick could walk the story through several phases at once.
            REQUIRE_FALSE(PhaseTracker::EvaluateAdvance(Phase::Exposition, 100, 29.9f).has_value());
        }
    }

    SECTION("when the dwell exactly meets the floor")
    {
        SECTION("should allow the advance")
        {
            // The comparison is `<`, so the floor itself is enough. Pinned
            // because a `<=` here would silently add a whole tick of delay.
            REQUIRE(PhaseTracker::EvaluateAdvance(Phase::Exposition, 90, 30.0f) == Phase::RisingAction);
        }
    }

    SECTION("when the configured floor is negative")
    {
        const ConfiguredSettings negative{"[Director]\niMinPhaseDurationSeconds=-5\niAdvanceThresholdExposition=45\n"};

        SECTION("should treat it as no floor")
        {
            // Clamped at zero rather than compared directly, so a nonsense INI
            // value cannot make a zero dwell fail the comparison.
            REQUIRE(PhaseTracker::EvaluateAdvance(Phase::Exposition, 90, 0.0f) == Phase::RisingAction);
        }
    }
}

TEST_CASE("PhaseTracker::EvaluateAdvance thresholds", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: no dwell floor, and every threshold set to a
    // distinct value so a case that reads the wrong one fails rather than
    // passing on a coincidence. The two falling thresholds are the ones worth
    // being careful about — they compare the other way round.
    EngineMock engine;
    const ConfiguredSettings settings{"[Director]\n"
                                      "iMinPhaseDurationSeconds=0\n"
                                      "iAdvanceThresholdExposition=45\n"
                                      "iAdvanceThresholdRisingAction=80\n"
                                      "iAdvanceThresholdClimax=60\n"
                                      "iAdvanceThresholdFallingAction=30\n"
                                      "iAdvanceThresholdResolution=25\n"};

    SECTION("when the story is in Exposition")
    {
        SECTION("should rise into RisingAction at the threshold")
        {
            REQUIRE(PhaseTracker::EvaluateAdvance(Phase::Exposition, 45, 0.0f) == Phase::RisingAction);
        }

        SECTION("should stay below it")
        {
            REQUIRE_FALSE(PhaseTracker::EvaluateAdvance(Phase::Exposition, 44, 0.0f).has_value());
        }
    }

    SECTION("when the story is in RisingAction")
    {
        SECTION("should rise into Climax at the threshold")
        {
            REQUIRE(PhaseTracker::EvaluateAdvance(Phase::RisingAction, 80, 0.0f) == Phase::Climax);
        }

        SECTION("should stay below it")
        {
            // 79 is over the Exposition threshold, so a case reading the wrong
            // arm's setting would advance here.
            REQUIRE_FALSE(PhaseTracker::EvaluateAdvance(Phase::RisingAction, 79, 0.0f).has_value());
        }
    }

    SECTION("when the story is at the Climax")
    {
        SECTION("should fall into FallingAction at the threshold")
        {
            // Note the direction: the story leaves the Climax when tension
            // drops back down, not when it rises further.
            REQUIRE(PhaseTracker::EvaluateAdvance(Phase::Climax, 60, 0.0f) == Phase::FallingAction);
        }

        SECTION("should stay above it")
        {
            REQUIRE_FALSE(PhaseTracker::EvaluateAdvance(Phase::Climax, 61, 0.0f).has_value());
        }
    }

    SECTION("when the story is in FallingAction")
    {
        SECTION("should fall into Resolution at the threshold")
        {
            REQUIRE(PhaseTracker::EvaluateAdvance(Phase::FallingAction, 30, 0.0f) == Phase::Resolution);
        }

        SECTION("should stay above it")
        {
            REQUIRE_FALSE(PhaseTracker::EvaluateAdvance(Phase::FallingAction, 31, 0.0f).has_value());
        }
    }

    SECTION("when the story is in Resolution")
    {
        SECTION("should rise into a new Exposition at the threshold")
        {
            REQUIRE(PhaseTracker::EvaluateAdvance(Phase::Resolution, 25, 0.0f) == Phase::Exposition);
        }

        SECTION("should stay below it")
        {
            REQUIRE_FALSE(PhaseTracker::EvaluateAdvance(Phase::Resolution, 24, 0.0f).has_value());
        }
    }

    SECTION("when the phase is out of range")
    {
        SECTION("should refuse to advance")
        {
            // Only reachable from a corrupt co-save byte that got past the
            // loader's own check; refusing is the quiet, harmless answer.
            REQUIRE_FALSE(PhaseTracker::EvaluateAdvance(static_cast<Phase>(99), 100, 1000.0f).has_value());
        }
    }
}

TEST_CASE("PhaseTracker::OutgoingDirection", "[PhaseTracker][engine]")
{
    SECTION("when the phase leaves by rising")
    {
        SECTION("should say raise")
        {
            REQUIRE(PhaseTracker::OutgoingDirection(Phase::Exposition) == Direction::Raise);
            REQUIRE(PhaseTracker::OutgoingDirection(Phase::RisingAction) == Direction::Raise);
            REQUIRE(PhaseTracker::OutgoingDirection(Phase::Resolution) == Direction::Raise);
        }
    }

    SECTION("when the phase leaves by falling")
    {
        SECTION("should say lower")
        {
            // This has to agree with EvaluateAdvance's comparison direction for
            // the same phase, or BeatSystem filters the registry down to beats
            // that push tension the wrong way and the phase never ends.
            REQUIRE(PhaseTracker::OutgoingDirection(Phase::Climax) == Direction::Lower);
            REQUIRE(PhaseTracker::OutgoingDirection(Phase::FallingAction) == Direction::Lower);
        }
    }

    SECTION("when the phase is out of range")
    {
        SECTION("should default to raise")
        {
            REQUIRE(PhaseTracker::OutgoingDirection(static_cast<Phase>(99)) == Direction::Raise);
        }
    }
}

TEST_CASE("PhaseTracker::AdvanceTo", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: a tracker that has been running a while in
    // Climax, loaded from a co-save so the dwell and the anchor are both
    // plainly non-zero. Each case then advances and checks what moved.
    EngineMock engine;
    const FreshTracker tracker;

    engine.cosave.readable = LoadedPayload(static_cast<std::uint8_t>(Phase::Climax), 500.0f, kOldAnchor);
    engine.cosave.readCursor = 0;
    PhaseTracker::OnLoad(FakeInterface(), 3, 0);

    SECTION("when the Director advances the phase")
    {
        SECTION("should report the new phase")
        {
            PhaseTracker::AdvanceTo(Phase::FallingAction);
            REQUIRE(PhaseTracker::Get() == Phase::FallingAction);
        }

        SECTION("should restart the dwell clock")
        {
            // The loaded dwell is 500 seconds, so a tracker that failed to zero
            // it would read as 500-and-change rather than as a fresh phase.
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() > 499.0f);
            PhaseTracker::AdvanceTo(Phase::FallingAction);
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() < 1.0f);
        }

        SECTION("should re-anchor the phase-entered moment")
        {
            // The anchor is what filters the SkyrimNet event tail down to
            // events from the current phase. Leaving it at the loaded value
            // would let events the previous phase already consumed justify
            // advancing again.
            REQUIRE(PhaseTracker::PhaseEnteredAtRealTime() == kOldAnchor);
            PhaseTracker::AdvanceTo(Phase::FallingAction);
            REQUIRE(PhaseTracker::PhaseEnteredAtRealTime() > kRecentEpochSeconds);
        }

        SECTION("should notify every event log")
        {
            // Each log holds a per-phase window of its own and clears it here.
            // A log that is not told keeps reporting last phase's weather.
            PhaseTracker::AdvanceTo(Phase::FallingAction);
            REQUIRE(NarrativeEngine::Testing::EventLogSpies().combatPhaseAdvances.load() == 1);
            REQUIRE(NarrativeEngine::Testing::EventLogSpies().travelPhaseAdvances.load() == 1);
        }
    }
}

TEST_CASE("PhaseTracker::Reset", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: a tracker sitting in a late phase with an
    // old anchor, so a reset that failed to move either would be visible.
    EngineMock engine;
    const FreshTracker tracker;

    engine.cosave.readable = LoadedPayload(static_cast<std::uint8_t>(Phase::Resolution), 500.0f, kOldAnchor);
    engine.cosave.readCursor = 0;
    PhaseTracker::OnLoad(FakeInterface(), 3, 0);

    SECTION("when reset with no argument")
    {
        SECTION("should return to Exposition")
        {
            PhaseTracker::Reset();
            REQUIRE(PhaseTracker::Get() == Phase::Exposition);
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() < 1.0f);
        }

        SECTION("should anchor the phase-entered moment to now")
        {
            // A fresh new game filters events to "since now", i.e. nothing yet.
            // Leaving the anchor at zero would have the first evaluation
            // consider every event the session has ever seen.
            PhaseTracker::Reset();
            REQUIRE(PhaseTracker::PhaseEnteredAtRealTime() > kRecentEpochSeconds);
        }
    }

    SECTION("when reset to a named phase")
    {
        SECTION("should adopt it")
        {
            PhaseTracker::Reset(Phase::Climax);
            REQUIRE(PhaseTracker::Get() == Phase::Climax);
        }
    }

    SECTION("when the plugin reverts")
    {
        SECTION("should return to Exposition")
        {
            PhaseTracker::OnRevert();
            REQUIRE(PhaseTracker::Get() == Phase::Exposition);
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() < 1.0f);
        }
    }
}

TEST_CASE("PhaseTracker::TimeInPhaseSeconds", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: an unpaused game and a freshly reset clock.
    EngineMock engine;
    const FreshTracker tracker;

    SECTION("when the game is running")
    {
        SECTION("should accumulate real time")
        {
            Elapse();
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() > kHalfSleepSeconds);
        }

        SECTION("should keep growing without a tick")
        {
            // The getter samples the clock itself rather than reporting the
            // value as of the last Tick, so a caller between ticks sees a
            // current dwell rather than a stale one.
            Elapse();
            const float first = PhaseTracker::TimeInPhaseSeconds();
            Elapse();
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() > first);
        }
    }

    SECTION("when the game is paused")
    {
        engine.ui.gameIsPaused = true;

        SECTION("should freeze the clock")
        {
            // Menus, the console and dialogue all pause. Counting that time
            // would let a phase time out while the player was reading their
            // inventory.
            Elapse();
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() < kHalfSleepSeconds);
        }

        SECTION("should freeze it across a tick too")
        {
            // Tick runs from the plugin thread while the player sits in a
            // menu, and shares the sampler with the getter. A tick that rolled
            // the interval in would defeat the pause check on the read path.
            Elapse();
            TickOnPluginThread();
            Elapse();
            REQUIRE(PhaseTracker::TimeInPhaseSeconds() < kHalfSleepSeconds);
        }
    }
}

TEST_CASE("PhaseTracker::OnSave", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: a tracker in a phase that is not the default
    // one, so a writer that saved a constant would be visible.
    EngineMock engine;
    const FreshTracker tracker;
    PhaseTracker::AdvanceTo(Phase::FallingAction);

    SECTION("when the record opens")
    {
        PhaseTracker::OnSave(FakeInterface());

        SECTION("should stamp the frozen record type and version")
        {
            // The type is frozen by the header; changing it orphans every
            // previously saved payload. The version is 3 because the anchor
            // field is written, and the loader keys its layout off it.
            REQUIRE(engine.cosave.opened.size() == 1);
            REQUIRE(engine.cosave.opened[0].type == PhaseTracker::kRecordTypeId);
            REQUIRE(engine.cosave.opened[0].version == 3);
        }

        SECTION("should write the phase, the dwell and the anchor")
        {
            REQUIRE(engine.cosave.written.size() == sizeof(std::uint8_t) + sizeof(float) + sizeof(double));
            REQUIRE(static_cast<std::uint8_t>(engine.cosave.written[0])
                    == static_cast<std::uint8_t>(Phase::FallingAction));
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            // Writing into a record that was never opened corrupts whichever
            // record the co-save is actually positioned in.
            PhaseTracker::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing")
        {
            PhaseTracker::OnSave(nullptr);
            REQUIRE(engine.cosave.opened.empty());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}

TEST_CASE("PhaseTracker::OnLoad restores a saved phase", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: a well-formed current-version record for a
    // phase and dwell that no default would produce.
    EngineMock engine;
    const FreshTracker tracker;

    engine.cosave.readable = LoadedPayload(static_cast<std::uint8_t>(Phase::Climax), 123.5f, kOldAnchor);
    engine.cosave.readCursor = 0;

    SECTION("when the record is the current version")
    {
        PhaseTracker::OnLoad(FakeInterface(), 3, 0);

        SECTION("should restore the phase")
        {
            REQUIRE(PhaseTracker::Get() == Phase::Climax);
        }

        SECTION("should restore the dwell")
        {
            // Restored as a base the clock keeps adding to, not reset — a phase
            // the player has already been in for two minutes should not have to
            // earn its dwell floor again after every load.
            const float dwell = PhaseTracker::TimeInPhaseSeconds();
            REQUIRE(dwell >= 123.5f);
            REQUIRE(dwell < 124.5f);
        }

        SECTION("should restore the phase-entered anchor")
        {
            REQUIRE(PhaseTracker::PhaseEnteredAtRealTime() == kOldAnchor);
        }
    }

    SECTION("when the record was written by this build")
    {
        PhaseTracker::AdvanceTo(Phase::RisingAction);
        PhaseTracker::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        PhaseTracker::Reset();

        SECTION("should round-trip through OnSave")
        {
            // The writer and the reader are the pair that has to agree; either
            // one alone can look right while the save is unloadable.
            PhaseTracker::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(PhaseTracker::Get() == Phase::RisingAction);
        }
    }

    SECTION("when the record predates the anchor field")
    {
        engine.cosave.readable = LoadedPayloadV1(static_cast<std::uint8_t>(Phase::Climax), 123.5f);
        engine.cosave.readCursor = 0;

        SECTION("should restore the phase and leave the anchor open")
        {
            // Zero means "no cutoff", so the first evaluation after loading an
            // old save considers everything rather than nothing. Failing the
            // other way would make the Director mute until the next advance.
            PhaseTracker::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Climax);
            REQUIRE(PhaseTracker::PhaseEnteredAtRealTime() == 0.0);
        }
    }

    SECTION("when the record carries the superseded game-time anchor")
    {
        SECTION("should consume the field and leave the anchor open")
        {
            // v2 wrote a cumulative-game-seconds value into the same eight
            // bytes v3 uses for a Unix-epoch one. Reading it as an anchor would
            // put the cutoff somewhere in 1970, which passes everything; the
            // module discards it instead and lands on the same 0 as v1.
            PhaseTracker::OnLoad(FakeInterface(), 2, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Climax);
            REQUIRE(PhaseTracker::PhaseEnteredAtRealTime() == 0.0);
        }
    }
}

TEST_CASE("PhaseTracker::OnLoad rejects a record it cannot trust", "[PhaseTracker][engine]")
{
    // Happy path, re-run per leaf: the tracker is put into a late phase first,
    // so "fell back to defaults" is a state no case could reach by accident.
    EngineMock engine;
    const FreshTracker tracker;
    PhaseTracker::AdvanceTo(Phase::Climax);

    SECTION("when the version is one no build ever wrote")
    {
        engine.cosave.readable = LoadedPayload(static_cast<std::uint8_t>(Phase::Resolution), 500.0f, kOldAnchor);
        engine.cosave.readCursor = 0;

        SECTION("should fall back to defaults")
        {
            // The payload is perfectly readable; only the version is wrong.
            // Reading it anyway would restore fields from the wrong offsets.
            PhaseTracker::OnLoad(FakeInterface(), 7, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Exposition);
        }
    }

    SECTION("when the record ends before the phase byte")
    {
        SECTION("should fall back to defaults")
        {
            PhaseTracker::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Exposition);
        }
    }

    SECTION("when the record ends part way through the dwell")
    {
        engine.cosave.readable = LoadedPayload(static_cast<std::uint8_t>(Phase::Resolution), 500.0f, kOldAnchor);
        engine.cosave.readable.resize(sizeof(std::uint8_t) + 2);
        engine.cosave.readCursor = 0;

        SECTION("should fall back to defaults")
        {
            // A torn record. Half a float is not a dwell, and accepting one
            // would put a garbage number into the phase-timeout comparison.
            PhaseTracker::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Exposition);
        }
    }

    SECTION("when the phase byte is not a phase")
    {
        engine.cosave.readable = LoadedPayload(9, 500.0f, kOldAnchor);
        engine.cosave.readCursor = 0;

        SECTION("should fall back to defaults")
        {
            // Everything downstream indexes the name table and the threshold
            // switch by this byte, and both of those degrade quietly rather
            // than crashing, so the check has to happen here.
            PhaseTracker::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Exposition);
        }
    }

    SECTION("when the anchor field is truncated")
    {
        engine.cosave.readable = LoadedPayload(static_cast<std::uint8_t>(Phase::Resolution), 500.0f, kOldAnchor);
        engine.cosave.readable.resize(engine.cosave.readable.size() - 4);
        engine.cosave.readCursor = 0;

        SECTION("should keep the phase and leave the anchor open")
        {
            // The phase and dwell read cleanly, so there is no reason to throw
            // them away over a trailing field that has a safe default.
            PhaseTracker::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Resolution);
            REQUIRE(PhaseTracker::PhaseEnteredAtRealTime() == 0.0);
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should leave the live state alone")
        {
            // Distinct from every other rejection above: with no interface
            // there is nothing to have gone wrong, so resetting would discard
            // good state rather than protect anything.
            PhaseTracker::OnLoad(nullptr, 3, 0);
            REQUIRE(PhaseTracker::Get() == Phase::Climax);
        }
    }
}
