#include <MCMEventSink.h>

#include <AsyncDispatch.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <Settings.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

// Tests for the sink that notices an MCM setting change.
//
// This is the only path by which anything the player changes in the MCM reaches
// the running plugin. MCM Helper writes its override INI and fires a ModEvent;
// if the sink misses it, every MCM setting silently stops taking effect until
// the next load, and nothing anywhere reports a problem. That failure has cost
// diagnosis time before, which is why the module traces every event it sees
// whether it matched or not.
//
// The sink itself lives in an anonymous namespace, so it is reached the way the
// Papyrus VM reaches it: EngineMock's ModEvent source is a REAL BSTEventSource,
// and both AddEventSink and SendEvent are inline header code, so registering
// and dispatching run the engine's own machinery over storage the harness owns.
//
// Initialize latches an atomic on success and never unlatches, so exactly one
// case in a process can observe the unregistered state. That case skips itself
// if another got there first; under ctest each TEST_CASE has its own process.

namespace
{
    namespace MCMEventSink = NarrativeEngine::MCMEventSink;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace Settings = NarrativeEngine::Settings;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr auto kTimeout = std::chrono::seconds{5};

    // Must match the string _ne_MCM.psc passes to SendModEvent, and the one the
    // sink filters on. Spelled out here rather than shared with production on
    // purpose: the whole point is that the two agree, and a shared constant
    // would make any rename agree with itself.
    constexpr const char* kEventName = "_ne_DashboardHotkeyChanged";

    const std::filesystem::path kMcmIni{"Data/MCM/Settings/NarrativeEngine.ini"};

    // Writes the override INI MCM Helper would have written just before firing
    // its event, and removes it again on the way out. Only the file is removed;
    // a real install has the directories.
    struct McmOverride
    {
        explicit McmOverride(const std::string& body)
        {
            std::filesystem::create_directories(kMcmIni.parent_path());
            std::ofstream out{kMcmIni, std::ios::binary | std::ios::trunc};
            out << body;
            out.close();
        }

        ~McmOverride()
        {
            std::error_code ec;
            std::filesystem::remove(kMcmIni, ec);
            Settings::Load();
        }

        McmOverride(const McmOverride&) = delete;
        McmOverride& operator=(const McmOverride&) = delete;
    };

    // Sends a ModEvent through the source the sink registered on, exactly as
    // the Papyrus VM does.
    void SendModEvent(const char* eventName)
    {
        SKSE::ModCallbackEvent event{};
        event.eventName = eventName;
        event.strArg = "";
        event.numArg = 0.0f;
        event.sender = nullptr;
        EngineMock::ModEventSource().SendEvent(&event);
    }

    // The sink hands its work to AsyncDispatch, which hands it to the main
    // thread. Both hops have to complete before the settings reload is visible,
    // so this waits for the queue to drain rather than assuming it has.
    void DrainAsyncQueue()
    {
        std::promise<void> marker;
        auto reached = marker.get_future();
        AsyncDispatch::EnqueueWork([&](const NarrativeEngine::PluginThread::Token&) { marker.set_value(); });
        REQUIRE(reached.wait_for(kTimeout) == std::future_status::ready);
    }
} // namespace

TEST_CASE("MCMEventSink::Initialize when SKSE has no source", "[MCMEventSink][engine]")
{
    // The only case that needs the sink unregistered, and Initialize latches on
    // success. Skipping rather than asserting keeps a shuffled single-process
    // run honest about what it did not check.
    EngineMock engine;
    engine.tasks.runImmediately = true;
    engine.modEvents.sourcePresent = false;
    const ConfiguredSettings baseline{"[Dashboard]\niHotkeyDXSC=65\n"};

    SECTION("when the ModEvent source is unavailable")
    {
        MCMEventSink::Initialize();

        SECTION("should leave itself unregistered so a later call can retry")
        {
            // The latch is released on this path on purpose. SKSE messaging may
            // simply not be ready yet, and a sink that marked itself done would
            // never listen again for the rest of the session — every MCM change
            // silently ignored, with one error line at boot to explain it.
            //
            // Checked by making the retry actually work, rather than by
            // inspecting the latch: the retry succeeding is the only thing
            // anyone cares about, and it is what the latch exists for.
            const McmOverride override{"[Dashboard]\niHotkeyDXSC=88\n"};
            REQUIRE(Settings::Get().dashboardHotkeyDXSC == 65);

            engine.modEvents.sourcePresent = true;
            MCMEventSink::Initialize();
            AsyncDispatch::Start();
            SendModEvent(kEventName);
            DrainAsyncQueue();
            AsyncDispatch::Stop();

            REQUIRE(Settings::Get().dashboardHotkeyDXSC == 88);
        }
    }
}

TEST_CASE("MCMEventSink dispatches an MCM change", "[MCMEventSink][engine]")
{
    // Happy path, re-run per leaf: the source is up, the sink is registered,
    // AsyncDispatch is running and main-thread work runs inline. Settings start
    // from a plugin INI whose hotkey differs from the MCM override, so a reload
    // that did not happen is distinguishable from one that did.
    EngineMock engine;
    engine.tasks.runImmediately = true;
    const ConfiguredSettings baseline{"[Dashboard]\niHotkeyDXSC=65\n"};
    MCMEventSink::Initialize();
    AsyncDispatch::Start();

    SECTION("when the matching event arrives")
    {
        const McmOverride override{"[Dashboard]\niHotkeyDXSC=88\n"};
        REQUIRE(Settings::Get().dashboardHotkeyDXSC == 65);
        SendModEvent(kEventName);
        DrainAsyncQueue();

        SECTION("should re-read the MCM override")
        {
            // The whole contract. MCM Helper has already written the file by
            // the time the event lands, so the sink parses no payload — it just
            // has to trigger the read.
            REQUIRE(Settings::Get().dashboardHotkeyDXSC == 88);
        }
    }

    SECTION("when a different mod's event arrives")
    {
        const McmOverride override{"[Dashboard]\niHotkeyDXSC=88\n"};
        REQUIRE(Settings::Get().dashboardHotkeyDXSC == 65);
        SendModEvent("SomeOtherMod_SettingChanged");
        DrainAsyncQueue();

        SECTION("should ignore it")
        {
            // Every mod's ModEvents land on every sink. Reloading settings on
            // someone else's event would work by accident here and do real work
            // on every unrelated event in the game.
            REQUIRE(Settings::Get().dashboardHotkeyDXSC == 65);
        }
    }

    SECTION("when the override file is absent")
    {
        REQUIRE(Settings::Get().dashboardHotkeyDXSC == 65);
        SendModEvent(kEventName);
        DrainAsyncQueue();

        SECTION("should leave the plugin's own settings in place")
        {
            // A fresh install, or a player who has never opened the MCM page.
            // Clearing to defaults here would silently undo the plugin INI.
            REQUIRE(Settings::Get().dashboardHotkeyDXSC == 65);
        }
    }

    SECTION("when Initialize is called again")
    {
        SECTION("should register no second sink")
        {
            // Two sinks would apply the override twice per event — harmless
            // today, and a real problem the moment the handler stops being
            // idempotent.
            //
            // Worth knowing what actually holds this up: the module's own latch
            // is not the only guard, because BSTEventSource::AddEventSink
            // refuses a pointer it already holds. Deleting the latch leaves
            // this case passing. The latch still earns its place on the other
            // path — a registration that failed for want of a source must be
            // retryable, which the case above checks.
            const McmOverride override{"[Dashboard]\niHotkeyDXSC=88\n"};
            MCMEventSink::Initialize();
            MCMEventSink::Initialize();
            SendModEvent(kEventName);
            DrainAsyncQueue();
            REQUIRE(engine.tasks.queued == 1);
        }
    }

    AsyncDispatch::Stop();
}
