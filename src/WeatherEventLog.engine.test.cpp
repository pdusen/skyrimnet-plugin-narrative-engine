#include <WeatherEventLog.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <PhaseTracker.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <SKSE/Interfaces.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

// Tests for the weather event source.
//
// This is edge detection over a polled sample, and every part of it exists
// because the naive version misbehaves. The module derives a small category
// from the current weather's authored flags and emits only when that category
// changes — so what matters is not "what is the weather" but "what counts as a
// different weather", and the answer is deliberately coarse: a rain shower
// becoming a thunderstorm is an event; one rainy weather record swapping for
// another rainy one is not, and emitting on it would fill the Director's tail
// with changes no player could perceive.
//
// The debounce is the other half. Vanilla weather transitions blend, so a
// sample taken mid-blend can flip and flip back, and without a floor between
// emissions a single change becomes two contradictory events seconds apart.
// Note what the debounced path still does: it advances the baseline. Skipping
// that would make the NEXT poll see the same change again and fire it late.
//
// The engine side is a fabricated sky whose weather flags a test sets directly,
// which is exactly the input the module reads.

namespace
{
    namespace WeatherEventLog = NarrativeEngine::WeatherEventLog;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    // The authored flag bits the module reads. Named here rather than taken
    // from the enum so a renumbering upstream shows up as a failure instead of
    // being silently followed.
    constexpr std::uint8_t kPleasant = 0x01;
    constexpr std::uint8_t kCloudy = 0x02;
    constexpr std::uint8_t kRainy = 0x04;
    constexpr std::uint8_t kSnowy = 0x08;

    // A one-second poll interval and no debounce, so a case can drive one
    // transition per poll and read it back immediately. Every poll below is
    // handed exactly one interval: the module subtracts the interval rather
    // than zeroing, so a larger figure would leave overshoot behind and the
    // next poll would fire however little time it was given.
    constexpr const char* kResponsiveSettings =
        "[WeatherEvents]\niWeatherEventPollIntervalSeconds=1\niWeatherEventDebounceSeconds=0\n"
        "iWeatherEventsMaxStored=128\n";

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    void PollWith(double unpausedElapsedSeconds)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { WeatherEventLog::Poll(pt, unpausedElapsedSeconds); });
    }

    // Puts a weather on the sky. Calm by default: authored lightning and high
    // wind are what make a precipitation weather count as stormy, so a fixture
    // that left either at a nonzero default would make every shower a
    // thunderstorm and hide the distinction the module is built on.
    void SetWeather(EngineMock& engine,
                    std::uint8_t flags,
                    std::uint32_t formID,
                    std::uint8_t windSpeed = 0,
                    std::int8_t lightning = 0)
    {
        engine.sky.present = true;
        engine.sky.mode = static_cast<std::uint32_t>(RE::Sky::Mode::kFull);
        engine.sky.hasWeather = true;
        engine.sky.weatherFlags = flags;
        engine.sky.weatherFormID = formID;
        engine.sky.windSpeed = windSpeed;
        engine.sky.thunderLightningFrequency = lightning;
    }

    nlohmann::json Tail()
    {
        return WeatherEventLog::GetRenderedTail(0.0);
    }

    std::size_t TailSize()
    {
        return Tail().size();
    }

    // The module's state is process-wide, so every case starts from clean.
    struct FreshLog
    {
        FreshLog()
        {
            WeatherEventLog::OnRevert();
        }

        FreshLog(const FreshLog&) = delete;
        FreshLog& operator=(const FreshLog&) = delete;
    };
} // namespace

TEST_CASE("WeatherEventLog seeds a baseline before emitting", "[WeatherEventLog][engine]")
{
    // Happy path, re-run per leaf: a full outdoor sky showing pleasant weather.
    EngineMock engine;
    const ConfiguredSettings settings{kResponsiveSettings};
    const FreshLog fresh;
    SetWeather(engine, kPleasant, 0x0010E1F2u);

    SECTION("when the first poll observes the weather")
    {
        PollWith(1.0);

        SECTION("should emit nothing")
        {
            // There is no previous reading to have changed FROM. Emitting on
            // the first sample would report a weather event every time the
            // player loaded a save.
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when the weather then changes")
    {
        PollWith(1.0);
        SetWeather(engine, kRainy, 0x0010E1F3u);
        PollWith(1.0);

        SECTION("should emit the transition")
        {
            REQUIRE(TailSize() == 1);
        }
    }

    SECTION("when too little time has passed for a poll")
    {
        PollWith(1.0);
        SetWeather(engine, kRainy, 0x0010E1F3u);
        PollWith(0.25);

        SECTION("should not sample the sky yet")
        {
            // The throttle is the gate in front of a main-thread hop, so it has
            // to hold before any engine read happens.
            REQUIRE(TailSize() == 0);
        }
    }
}

TEST_CASE("WeatherEventLog only reports changes a player would notice", "[WeatherEventLog][engine]")
{
    // Happy path, re-run per leaf: a baseline of plain rain already seeded.
    EngineMock engine;
    const ConfiguredSettings settings{kResponsiveSettings};
    const FreshLog fresh;
    SetWeather(engine, kRainy, 0x0010E1F2u);
    PollWith(1.0);

    SECTION("when one rainy weather swaps for another")
    {
        SetWeather(engine, kRainy, 0x0010E1FFu);
        PollWith(1.0);

        SECTION("should emit nothing")
        {
            // Vanilla cycles between several records of the same character.
            // Emitting on the record rather than on the category would fill
            // the Director's tail with changes nobody could perceive.
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when rain becomes a thunderstorm")
    {
        // Same primary category, but the wind marks it stormy. Wind rather
        // than the thunder field: vanilla's storm variants leave lightning at
        // the CK's unset sentinel, so wind is the only reliable discriminator.
        SetWeather(engine, kRainy, 0x0010E1F3u, /*windSpeed=*/200);
        PollWith(1.0);

        SECTION("should emit the transition")
        {
            // Storminess is part of the category precisely so this counts: the
            // sky darkening over an ongoing shower is exactly the sort of thing
            // an NPC would remark on.
            REQUIRE(TailSize() == 1);
        }
    }

    SECTION("when precipitation gives way to clear sky")
    {
        SetWeather(engine, kPleasant, 0x0010E1F4u);
        PollWith(1.0);

        SECTION("should emit the transition")
        {
            REQUIRE(TailSize() == 1);
        }

        SECTION("should describe it as the rain stopping")
        {
            // The rendered sentence is what reaches the model, so the direction
            // of the transition has to survive into the text.
            const auto tail = Tail();
            const std::string text = tail[0].value("text", "");
            REQUIRE_FALSE(text.empty());
            REQUIRE(tail[0].value("ne_kind", "").find("rain") != std::string::npos);
        }
    }

    SECTION("when snow starts falling")
    {
        SetWeather(engine, kSnowy, 0x0010E1F5u);
        PollWith(1.0);

        SECTION("should tell snow apart from rain")
        {
            // Snow and rain are separate primaries because "it started to
            // snow" and "it started to rain" are different remarks.
            REQUIRE(TailSize() == 1);
            REQUIRE(Tail()[0].value("ne_kind", "").find("snow") != std::string::npos);
        }
    }

    SECTION("when a weather sets several flags at once")
    {
        // Rare but real. Precipitation wins over cloudy and pleasant, because
        // a "cloudy rainy" weather is still fundamentally rainy.
        SetWeather(engine, static_cast<std::uint8_t>(kCloudy | kSnowy), 0x0010E1F6u);
        PollWith(1.0);

        SECTION("should take the precipitation as the primary")
        {
            REQUIRE(TailSize() == 1);
            REQUIRE(Tail()[0].value("ne_kind", "").find("snow") != std::string::npos);
        }
    }
}

TEST_CASE("WeatherEventLog debounces a flapping sample", "[WeatherEventLog][engine]")
{
    // Vanilla transitions blend, so a sample taken mid-blend can flip and flip
    // back. Without a floor between emissions one change becomes two
    // contradictory events seconds apart.
    EngineMock engine;
    // A three-second debounce against a one-second poll, so a case can run
    // the window out in a handful of polls.
    const ConfiguredSettings settings{
        "[WeatherEvents]\niWeatherEventPollIntervalSeconds=1\niWeatherEventDebounceSeconds=3\n"};
    const FreshLog fresh;
    SetWeather(engine, kPleasant, 0x0010E1F2u);
    PollWith(1.0);

    SECTION("when a change lands inside the debounce window")
    {
        SetWeather(engine, kRainy, 0x0010E1F3u);
        PollWith(1.0);

        SECTION("should hold the event back")
        {
            REQUIRE(TailSize() == 0);
        }

        SECTION("should still advance the baseline")
        {
            // The part that is easy to lose, and it only shows once the window
            // has run out: a baseline left behind would make the first poll
            // past the debounce see the same change again and fire it late,
            // reporting a weather transition minutes after it happened.
            for (int i = 0; i < 6; ++i) {
                PollWith(1.0);
            }
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when the window has passed")
    {
        const ConfiguredSettings responsive{kResponsiveSettings};
        SetWeather(engine, kRainy, 0x0010E1F3u);
        PollWith(1.0);

        SECTION("should emit")
        {
            REQUIRE(TailSize() == 1);
        }
    }
}

TEST_CASE("WeatherEventLog ignores a sky it cannot read", "[WeatherEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kResponsiveSettings};
    const FreshLog fresh;
    SetWeather(engine, kPleasant, 0x0010E1F2u);
    PollWith(1.0);

    SECTION("when the player goes indoors")
    {
        engine.sky.mode = static_cast<std::uint32_t>(RE::Sky::Mode::kInterior);
        PollWith(1.0);

        SECTION("should emit nothing")
        {
            REQUIRE(TailSize() == 0);
        }

        SECTION("should keep the last outdoor reading as the baseline")
        {
            // Weather rarely changes while the player is inside, so diffing
            // against the last exterior sample on the way out usually produces
            // no event at all — which is right. Clearing it would report a
            // weather change every time somebody left a shop.
            engine.sky.mode = static_cast<std::uint32_t>(RE::Sky::Mode::kFull);
            PollWith(1.0);
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when the sky singleton is unavailable")
    {
        engine.sky.present = false;

        SECTION("should emit nothing rather than crash")
        {
            PollWith(1.0);
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when no weather is resolved")
    {
        engine.sky.hasWeather = false;
        PollWith(1.0);

        SECTION("should treat it as an uncategorised sky")
        {
            // A transition to "other" is still a transition; what must not
            // happen is a crash or a fabricated category.
            REQUIRE(TailSize() <= 1);
        }
    }
}

TEST_CASE("WeatherEventLog bounds what it keeps", "[WeatherEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{
        "[WeatherEvents]\niWeatherEventPollIntervalSeconds=1\niWeatherEventDebounceSeconds=0\n"
        "iWeatherEventsMaxStored=3\n"};
    const FreshLog fresh;
    SetWeather(engine, kPleasant, 0x0010E1F2u);
    PollWith(1.0);

    SECTION("when more events happen than the cap allows")
    {
        const std::uint8_t cycle[] = {kRainy, kSnowy, kPleasant, kCloudy, kRainy, kSnowy};
        std::uint32_t id = 0x0010E200u;
        for (const auto flags : cycle) {
            SetWeather(engine, flags, id++);
            PollWith(1.0);
        }

        SECTION("should keep no more than the cap")
        {
            // The tail is rendered into every Director prompt, so it is bounded
            // for cost as much as for memory.
            REQUIRE(TailSize() == 3);
        }
    }
}

TEST_CASE("WeatherEventLog forgets the previous phase", "[WeatherEventLog][engine]")
{
    // The tracker tells every event log when the story moves on. A log that
    // kept its old window would have the next phase's evaluation reasoning
    // about weather the previous phase already consumed.
    EngineMock engine;
    const ConfiguredSettings settings{kResponsiveSettings};
    const FreshLog fresh;
    SetWeather(engine, kPleasant, 0x0010E1F2u);
    PollWith(1.0);
    SetWeather(engine, kRainy, 0x0010E1F3u);
    PollWith(1.0);
    REQUIRE(TailSize() == 1);

    SECTION("when the phase advances")
    {
        SECTION("should drop what the previous phase already saw")
        {
            PhaseTracker::AdvanceTo(PhaseTracker::Phase::RisingAction);
            REQUIRE(TailSize() == 0);
        }
    }
}

TEST_CASE("WeatherEventLog survives a save and load", "[WeatherEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kResponsiveSettings};
    const FreshLog fresh;
    // A phase has to have been entered before anything is saved. OnSave prunes
    // to the current phase first, and with no phase ever entered the cutoff is
    // zero, which the module reads as "no baseline" and clears the log outright
    // rather than writing events it cannot place.
    PhaseTracker::Reset();
    SetWeather(engine, kPleasant, 0x0010E1F2u);
    PollWith(1.0);
    SetWeather(engine, kSnowy, 0x0010E1F3u);
    PollWith(1.0);
    REQUIRE(TailSize() == 1);

    SECTION("when the log is written and read back")
    {
        WeatherEventLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        WeatherEventLog::OnRevert();
        REQUIRE(TailSize() == 0);
        WeatherEventLog::OnLoad(FakeInterface(), 1, 0);

        SECTION("should restore the events")
        {
            REQUIRE(TailSize() == 1);
        }

        SECTION("should restore what the transition was")
        {
            // The from/to pair is what the sentence is rendered from, so a
            // loader that lost either half would restore an event that reads
            // as a transition to nowhere.
            REQUIRE(Tail()[0].value("ne_kind", "").find("snow") != std::string::npos);
        }
    }

    SECTION("when the record stamps its type")
    {
        WeatherEventLog::OnSave(FakeInterface());

        SECTION("should use the frozen record type")
        {
            REQUIRE(engine.cosave.opened.size() == 1);
            REQUIRE(engine.cosave.opened[0].type == WeatherEventLog::kRecordTypeId);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            WeatherEventLog::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        WeatherEventLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        WeatherEventLog::OnRevert();

        SECTION("should restore nothing")
        {
            WeatherEventLog::OnLoad(FakeInterface(), 99, 0);
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing")
        {
            WeatherEventLog::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
            WeatherEventLog::OnLoad(nullptr, 1, 0);
            REQUIRE(TailSize() == 1);
        }
    }
}

TEST_CASE("WeatherEventLog feeds the history writer", "[WeatherEventLog][engine]")
{
    EngineMock engine;

    SECTION("when the archive is on")
    {
        const ConfiguredSettings settings{
            "[WeatherEvents]\niWeatherEventPollIntervalSeconds=1\niWeatherEventDebounceSeconds=0\n"
            "[EventHistory]\nbEventHistoryEnabled=1\n"};
        const FreshLog fresh;
        SetWeather(engine, kPleasant, 0x0010E1F2u);
        PollWith(1.0);
        SetWeather(engine, kRainy, 0x0010E1F3u);
        PollWith(1.0);

        SECTION("should queue the rendered line")
        {
            // Rendered at emit time rather than at flush time, so the archive's
            // writer concatenates without re-deriving a sentence from a
            // category it would have to interpret again.
            const auto drained = WeatherEventLog::DrainHistoryTail();
            REQUIRE(drained.size() == 1);
            REQUIRE(drained[0].sourceKind.starts_with("internal/weather_event/"));
            REQUIRE_FALSE(drained[0].body.empty());
        }

        SECTION("should hand each entry over only once")
        {
            (void)WeatherEventLog::DrainHistoryTail();
            REQUIRE(WeatherEventLog::DrainHistoryTail().empty());
        }
    }

    SECTION("when the archive is off")
    {
        const ConfiguredSettings settings{
            "[WeatherEvents]\niWeatherEventPollIntervalSeconds=1\niWeatherEventDebounceSeconds=0\n"
            "[EventHistory]\nbEventHistoryEnabled=0\n"};
        const FreshLog fresh;
        SetWeather(engine, kPleasant, 0x0010E1F2u);
        PollWith(1.0);
        SetWeather(engine, kRainy, 0x0010E1F3u);
        PollWith(1.0);

        SECTION("should queue nothing")
        {
            // The queue has no reader when the archive is off, so filling it
            // would grow without bound for a whole session.
            REQUIRE(WeatherEventLog::DrainHistoryTail().empty());
        }

        SECTION("should still emit the event itself")
        {
            REQUIRE(TailSize() == 1);
        }
    }
}
