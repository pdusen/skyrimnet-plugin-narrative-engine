#include <MainThreadEngine.h>

#include <EngineMock.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

// Mocked-engine tests for the token-gated engine reads.
//
// These wrappers are the only place a worker thread ever learns anything about
// the world, and they exist to enforce two rules: the caller must hold a
// main-thread token, and what comes back must be plain data rather than a
// pointer into engine-owned memory. The first is a compile-time property and
// cannot be tested from here — code that violates it does not build. The second
// is what these cases check: every field of every snapshot arrives, and the
// absent-piece paths return a usable answer instead of dereferencing.
//
// Reaching the module needs the fabricated player, cell, location and sky. The
// cell and location are the first objects with a SECONDARY vtable, because
// `TESFullName::GetFullName` belongs to a base past the first; see
// testsupport/FakeVTable.h.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace Engine = NarrativeEngine::MainThreadEngine;
    namespace MainThread = NarrativeEngine::MainThread;
    namespace PluginThread = NarrativeEngine::PluginThread;

    constexpr std::uint32_t kActorFormID = 0x0001A6A0u;

    // The wrappers take a main-thread token, which only a dispatcher can make.
    // Going through the real ones keeps the test honest about how production
    // reaches these functions, and gets the Main thread role set on the way.
    template <class Fn> void OnMainThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            MainThread::Run(pt, [&](const MainThread::Token& mt) { body(mt); });
        });
    }
} // namespace

TEST_CASE("MainThreadEngine::ReadPlayerSnapshot", "[MainThreadEngine][engine]")
{
    // Happy path, re-run per leaf: a loaded player standing in a named interior
    // cell inside a named location. Each case removes one piece of that.
    EngineMock engine;

    SECTION("when the player is loaded")
    {
        Engine::PlayerSnapshot snapshot;
        OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadPlayerSnapshot(mt); });

        SECTION("should report the player's form id")
        {
            REQUIRE(snapshot.formID == engine.world.playerFormID);
        }

        SECTION("should report the player's position")
        {
            REQUIRE(snapshot.position.x == engine.world.playerX);
            REQUIRE(snapshot.position.y == engine.world.playerY);
            REQUIRE(snapshot.position.z == engine.world.playerZ);
        }

        SECTION("should report the current location")
        {
            REQUIRE(snapshot.locationFormID == engine.world.locationFormID);
            REQUIRE(snapshot.locationName == engine.world.locationName);
        }

        SECTION("should report the parent cell")
        {
            REQUIRE(snapshot.cellFormID == engine.world.cellFormID);
            REQUIRE(snapshot.cellName == engine.world.cellName);
        }

        SECTION("should report whether the cell is interior")
        {
            REQUIRE(snapshot.cellIsInterior);
        }
    }

    SECTION("when the player is in unmarked wilderness")
    {
        // No BGSLocation is a normal state, not an error: most of the map is
        // outside any named location.
        engine.world.playerHasLocation = false;
        Engine::PlayerSnapshot snapshot;
        OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadPlayerSnapshot(mt); });

        SECTION("should leave the location empty")
        {
            REQUIRE(snapshot.locationFormID == 0);
            REQUIRE(snapshot.locationName.empty());
        }

        SECTION("should still report the cell")
        {
            REQUIRE(snapshot.cellFormID == engine.world.cellFormID);
        }
    }

    SECTION("when the cell is exterior")
    {
        engine.world.cellIsInterior = false;

        SECTION("should say so")
        {
            Engine::PlayerSnapshot snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadPlayerSnapshot(mt); });
            REQUIRE_FALSE(snapshot.cellIsInterior);
        }
    }

    SECTION("when the player has no parent cell")
    {
        engine.world.playerHasCell = false;

        SECTION("should leave the cell empty")
        {
            Engine::PlayerSnapshot snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadPlayerSnapshot(mt); });
            REQUIRE(snapshot.cellFormID == 0);
            REQUIRE(snapshot.cellName.empty());
        }
    }

    SECTION("when the player singleton is unavailable")
    {
        // Pre-load and mid-teardown. Everything else is set, so a zeroed
        // snapshot can only mean the guard fired.
        engine.player.present = false;

        SECTION("should return a zeroed snapshot rather than crash")
        {
            Engine::PlayerSnapshot snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadPlayerSnapshot(mt); });
            REQUIRE(snapshot.formID == 0);
            REQUIRE(snapshot.cellFormID == 0);
            REQUIRE(snapshot.locationFormID == 0);
        }
    }
}

TEST_CASE("MainThreadEngine::LookupActor", "[MainThreadEngine][engine]")
{
    // Happy path: a named, living actor in the form table.
    EngineMock engine;
    (void)engine.AddActor(kActorFormID);

    SECTION("when the actor resolves and has a name")
    {
        std::optional<Engine::ActorSnapshot> snapshot;
        OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::LookupActor(mt, kActorFormID); });

        SECTION("should return a snapshot")
        {
            REQUIRE(snapshot.has_value());
        }

        SECTION("should report the display name")
        {
            REQUIRE(snapshot->displayName == engine.world.actorDisplayName);
        }

        SECTION("should report the liveness flags")
        {
            REQUIRE_FALSE(snapshot->isDead);
            REQUIRE_FALSE(snapshot->isDisabled);
        }
    }

    SECTION("when the actor is dead")
    {
        engine.forms.actorIsDead = true;

        SECTION("should still return a snapshot saying so")
        {
            // Death is reported, not filtered: a caller deciding whether to
            // use an actor needs to see it rather than get a bare nullopt it
            // cannot distinguish from a bad form id.
            std::optional<Engine::ActorSnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::LookupActor(mt, kActorFormID); });
            REQUIRE(snapshot.has_value());
            REQUIRE(snapshot->isDead);
        }
    }

    SECTION("when the actor is a player teammate")
    {
        engine.world.actorIsPlayerTeammate = true;

        SECTION("should say so")
        {
            std::optional<Engine::ActorSnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::LookupActor(mt, kActorFormID); });
            REQUIRE(snapshot->isPlayerTeammate);
        }
    }

    SECTION("when the form does not resolve")
    {
        SECTION("should return nothing")
        {
            std::optional<Engine::ActorSnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::LookupActor(mt, kActorFormID + 1); });
            REQUIRE_FALSE(snapshot.has_value());
        }
    }

    SECTION("when the actor has no display name")
    {
        // An unnamed actor is one the LLM cannot refer to, so it is filtered
        // here rather than surfacing as an empty name downstream.
        engine.world.actorDisplayName.clear();

        SECTION("should return nothing")
        {
            std::optional<Engine::ActorSnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::LookupActor(mt, kActorFormID); });
            REQUIRE_FALSE(snapshot.has_value());
        }
    }
}

TEST_CASE("MainThreadEngine::ReadCurrentSky", "[MainThreadEngine][engine]")
{
    // Happy path: a full outdoor sky with weather resolved.
    EngineMock engine;

    SECTION("when the sky is available")
    {
        std::optional<Engine::SkySnapshot> snapshot;
        OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadCurrentSky(mt); });

        SECTION("should return a snapshot")
        {
            REQUIRE(snapshot.has_value());
        }

        SECTION("should report the sky mode")
        {
            REQUIRE(snapshot->mode == Engine::SkyMode::Full);
        }

        SECTION("should report the current weather")
        {
            REQUIRE(snapshot->currentWeatherFormID == engine.sky.weatherFormID);
        }

        SECTION("should pass the weather flags through untouched")
        {
            // Deliberately raw: callers apply their own precedence over snow,
            // rain, pleasant and cloudy, so interpreting the byte here would
            // take that decision away from them.
            REQUIRE(snapshot->weatherFlags == engine.sky.weatherFlags);
        }

        SECTION("should report wind and thunder")
        {
            REQUIRE(snapshot->windSpeed == engine.sky.windSpeed);
            REQUIRE(snapshot->thunderLightningFrequency == engine.sky.thunderLightningFrequency);
        }
    }

    SECTION("when the sky is interior")
    {
        engine.sky.mode = static_cast<std::uint32_t>(RE::Sky::Mode::kInterior);

        SECTION("should report interior")
        {
            std::optional<Engine::SkySnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadCurrentSky(mt); });
            REQUIRE(snapshot->mode == Engine::SkyMode::Interior);
        }
    }

    SECTION("when only the sky dome is drawn")
    {
        engine.sky.mode = static_cast<std::uint32_t>(RE::Sky::Mode::kSkyDomeOnly);

        SECTION("should report sky-dome-only")
        {
            std::optional<Engine::SkySnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadCurrentSky(mt); });
            REQUIRE(snapshot->mode == Engine::SkyMode::SkyDomeOnly);
        }
    }

    SECTION("when the sky mode is one the wrapper does not name")
    {
        // kNone, and anything past the enum. Should never appear at runtime,
        // but the default arm exists so an unexpected value reads as the safest
        // "not really outdoors" answer rather than as whatever it happened to
        // be. kNone is 0, which is a value the engine does use before load.
        engine.sky.mode = static_cast<std::uint32_t>(RE::Sky::Mode::kNone);

        SECTION("should fall back to interior")
        {
            std::optional<Engine::SkySnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadCurrentSky(mt); });
            REQUIRE(snapshot->mode == Engine::SkyMode::Interior);
        }
    }

    SECTION("when no weather is resolved")
    {
        engine.sky.hasWeather = false;

        SECTION("should leave the weather fields at zero")
        {
            std::optional<Engine::SkySnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadCurrentSky(mt); });
            REQUIRE(snapshot.has_value());
            REQUIRE(snapshot->currentWeatherFormID == 0);
            REQUIRE(snapshot->weatherFlags == 0);
        }
    }

    SECTION("when the sky singleton is unavailable")
    {
        engine.sky.present = false;

        SECTION("should return nothing")
        {
            std::optional<Engine::SkySnapshot> snapshot;
            OnMainThread([&](const MainThread::Token& mt) { snapshot = Engine::ReadCurrentSky(mt); });
            REQUIRE_FALSE(snapshot.has_value());
        }
    }
}
