#include <DebugNotify.h>

#include <AsyncDispatch.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <PluginThread.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <string>

// Tests for the in-game debug notices.
//
// These exist because most of what the beats do is invisible while it is
// happening: a visitor is warped in out of sight, a letter sits in the
// courier's pocket, a rumour spreads between NPCs who are nowhere near
// the player. A notice is the difference between "it never fired" and
// "it fired and I was looking the other way".
//
// Two things therefore matter and are pinned here. They must say nothing
// at all unless asked for -- they name factions and NPCs the character
// could not know about, so a player who has not turned them on must
// never see one. And the naming variant has to resolve the name at the
// point of DISPLAY, because its callers are on other threads where
// touching a form is not allowed.

namespace
{
    namespace DebugNotify = NarrativeEngine::DebugNotify;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr auto kTimeout = std::chrono::seconds{5};

    constexpr const char* kOn = "[General]\nbDebugNotifications=1\n";
    constexpr const char* kOff = "[General]\nbDebugNotifications=0\n";

    constexpr std::uint32_t kNpc = 0x00E00001u;

    // A post crosses to the plugin thread and then to the main thread,
    // so both hops have to finish before the notice is observable.
    void DrainAsyncQueue()
    {
        std::promise<void> marker;
        auto reached = marker.get_future();
        AsyncDispatch::EnqueueWork([&](const NarrativeEngine::PluginThread::Token&) { marker.set_value(); });
        REQUIRE(reached.wait_for(kTimeout) == std::future_status::ready);
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
    };
} // namespace

TEST_CASE("DebugNotify stays quiet unless it was asked for", "[DebugNotify][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kOff};
    const RunningDispatch dispatch;

    SECTION("when the setting is off")
    {
        DebugNotify::Post("something happened");
        DrainAsyncQueue();

        SECTION("should show nothing")
        {
            // The default, and the one that matters: these notices say
            // things the character has no way of knowing, so a player
            // who never enabled them must never see one.
            REQUIRE(engine.notifications.shown.empty());
        }
    }
}

TEST_CASE("DebugNotify shows what it was given", "[DebugNotify][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kOn};
    const RunningDispatch dispatch;

    SECTION("when the setting is on")
    {
        DebugNotify::Post("Ysolda is coming to see you");
        DrainAsyncQueue();

        SECTION("should show the text unchanged")
        {
            REQUIRE(engine.notifications.shown.size() == 1);
            REQUIRE(engine.notifications.shown.front() == "Ysolda is coming to see you");
        }
    }

    SECTION("when there is nothing to say")
    {
        DebugNotify::Post("");
        DrainAsyncQueue();

        SECTION("should not show an empty notice")
        {
            REQUIRE(engine.notifications.shown.empty());
        }
    }
}

TEST_CASE("DebugNotify names an actor it was only given the id of", "[DebugNotify][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kOn};
    const RunningDispatch dispatch;

    SECTION("when the id resolves")
    {
        engine.AddNPC(kNpc, "Faralda");
        DebugNotify::PostActorNamed(kNpc, "New rumor spreading from {}");
        DrainAsyncQueue();

        SECTION("should look the name up and fill it in")
        {
            // Deliberately not passed as a string by the caller: the
            // gossip simulation seeds rumours on its own thread, where
            // reading a form is not allowed, so only the id crosses.
            REQUIRE(engine.notifications.shown.size() == 1);
            REQUIRE(engine.notifications.shown.front() == "New rumor spreading from Faralda");
        }
    }

    SECTION("when the id resolves to nothing")
    {
        DebugNotify::PostActorNamed(0xDEADBEEF, "New rumor spreading from {}");
        DrainAsyncQueue();

        SECTION("should still say something rather than drop the notice")
        {
            REQUIRE(engine.notifications.shown.size() == 1);
            REQUIRE(engine.notifications.shown.front() == "New rumor spreading from someone");
        }
    }
}
