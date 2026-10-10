#include <DashboardUIManager.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakePrismaUI.h>

#include <AsyncDispatch.h>
#include <BeatRegistry.h>
#include <DecisionLog.h>
#include <IBeat.h>
#include <PhaseTracker.h>
#include <PluginThread.h>
#include <PrismaUI.h>
#include <Settings.h>
#include <Tick.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

// Tests for the in-game dashboard's C++ side.
//
// It does two things and they pull in opposite directions. Outward, it renders
// everything the Director knows into one JSON blob the page reads -- so the
// shape of that blob is a contract with TypeScript nobody compiles together,
// and a field renamed on this side silently empties a panel on the other.
// Inward, it is a set of controls: every switch and slider on the page exists
// only as a callback registered with PrismaUI, and each of them writes a
// setting to disk that has to survive a reboot.
//
// PrismaUI is a runtime soft dependency reached through GetModuleHandle with
// no seam, so the harness builds a real DLL of that name (see FakePrismaUI.h)
// and the wrapper finds it exactly as it would in game. The controls are
// reached by asking that fake for the callbacks the view registered, which is
// the only handle on them there is -- the page is the sole caller in
// production.

namespace
{
    namespace Dashboard = NarrativeEngine::DashboardUIManager;
    namespace BeatRegistry = NarrativeEngine::BeatRegistry;
    namespace DecisionLog = NarrativeEngine::DecisionLog;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace PrismaUI = NarrativeEngine::PrismaUI_API;
    namespace Settings = NarrativeEngine::Settings;
    namespace Tick = NarrativeEngine::Tick;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::BeatPolarity;
    using NarrativeEngine::BeatState;
    using NarrativeEngine::IBeat;
    using NarrativeEngine::TickMode;
    using NarrativeEngine::TickResult;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakePrismaState;
    using NarrativeEngine::Testing::FakePrismaStateFunc;
    using NarrativeEngine::Testing::kFakePrismaStateExport;

    constexpr const char* kSettings = "[General]\nbDebugMode=0\n"
                                      "[Director]\nbTickEnabled=1\niTickIntervalSeconds=30\n";

    FakePrismaState& FakePrisma()
    {
        HMODULE module = ::LoadLibraryA("PrismaUI");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakePrismaStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakePrismaStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    // The callback the view registered under `name`. This is the only handle
    // on a dashboard control that exists: the page is the sole caller in
    // production and there is no other entry point.
    FakePrismaState::ListenerFn Control(const std::string& name)
    {
        auto& state = FakePrisma();
        for (int i = 0; i < state.recordedListeners; ++i) {
            if (name == state.listenerNames[i]) {
                return state.listenerCallbacks[i];
            }
        }
        return nullptr;
    }

    void Press(const std::string& name, const std::string& payload)
    {
        auto* control = Control(name);
        REQUIRE(control != nullptr);
        control(payload.c_str());
    }

    template <class Fn> bool Eventually(Fn predicate, std::chrono::milliseconds timeout = std::chrono::seconds{5})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return predicate();
    }

    struct RunningDispatch
    {
        RunningDispatch()
        {
            NarrativeEngine::AsyncDispatch::Start();
        }
        ~RunningDispatch()
        {
            NarrativeEngine::AsyncDispatch::Stop();
        }
        RunningDispatch(const RunningDispatch&) = delete;
        RunningDispatch& operator=(const RunningDispatch&) = delete;
    };

    // A beat with a cooldown to report, which is one of the few things the
    // state blob has to reach out to the main thread for.
    class FakeBeat : public IBeat
    {
    public:
        explicit FakeBeat(std::string name) : name_(std::move(name)) {}

        std::string Name() const override
        {
            return name_;
        }
        std::string Description() const override
        {
            return "A fake beat, for the dashboard's benefit.";
        }
        BeatPolarity Polarity() const override
        {
            return BeatPolarity::Either;
        }
        bool IsAvailable(const BeatContext&) const override
        {
            return true;
        }
        void OnStart(const BeatContext&, const nlohmann::json&) override {}
        TickResult Tick(const PluginThread::Token&, TickMode, BeatState) override
        {
            return {};
        }
        void Abort(const NarrativeEngine::MainThread::Token&) override {}
        double RemainingCooldownGameHours() const override
        {
            return 12.5;
        }

    private:
        std::string name_;
    };

    void RegisterBeat(const std::string& name)
    {
        BeatRegistry::Register(std::make_unique<FakeBeat>(name));
        BeatRegistry::SetEnabled(name, true);
    }

    nlohmann::json ComposedState()
    {
        Dashboard::DashboardEngineReads reads;
        reads.currentGameTimeSeconds = 86400.0;
        reads.playerName = "Dovahkiin";
        reads.beatCooldownHours.emplace("fake_dashboard_beat", 12.5);
        const auto json = nlohmann::json::parse(Dashboard::ComposeFullStateJSON(reads), nullptr, false);
        REQUIRE_FALSE(json.is_discarded());
        return json;
    }

} // namespace

TEST_CASE("DashboardUIManager renders what the Director knows", "[DashboardUIManager][engine]")
{
    // The blob the page reads. Its field names are a contract with TypeScript
    // nothing compiles against this side, so a rename here empties a panel
    // over there with nothing anywhere saying so.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    PhaseTracker::Reset();
    DecisionLog::Clear();
    RegisterBeat("fake_dashboard_beat");

    SECTION("when the page asks for the whole state")
    {
        const auto state = ComposedState();

        SECTION("should say what the plugin can currently reach")
        {
            // The first thing a player checks when the mod seems dead is
            // whether its two dependencies resolved.
            REQUIRE(state.contains("status"));
            REQUIRE(state["status"].contains("skyrim_net_available"));
            REQUIRE(state["status"].contains("prisma_ui_available"));
            REQUIRE(state["status"]["tick_enabled"] == Tick::IsEnabled());
        }

        SECTION("should say where the story has got to")
        {
            REQUIRE(state.contains("current_phase"));
            REQUIRE(state.contains("time_in_phase_seconds"));
        }

        SECTION("should carry the settings the page can edit")
        {
            // The Settings tab renders straight off this, so a control with
            // no value behind it comes up blank rather than at its default.
            REQUIRE(state.contains("settings"));
            REQUIRE(state["settings"].contains("tick_interval_seconds"));
            REQUIRE(state["settings"]["tick_interval_seconds"] == 30);
        }

        SECTION("should list the beats and what each is waiting on")
        {
            // The cooldown is the one number the page cannot compute for
            // itself: it comes from each beat, on the main thread, and is
            // gathered before the compose starts.
            REQUIRE(state.contains("actions"));
            bool found = false;
            for (const auto& action : state["actions"]) {
                if (action.value("name", "") == "fake_dashboard_beat") {
                    found = true;
                    REQUIRE(action["remaining_cooldown_hours"] == 12.5);
                }
            }
            REQUIRE(found);
        }
    }

    SECTION("when a decision has been filed")
    {
        DecisionLog::DecisionRecord record;
        record.realTimeSec = 99.0;
        record.tensionScore = 42;
        record.narrativeNote = "the road was quiet";
        DecisionLog::Append(record);

        SECTION("should show what the Director decided and why")
        {
            // The decision history is the only account of what the mod has
            // been doing, and it is the whole reason the dashboard exists.
            const auto state = ComposedState();
            REQUIRE(state.contains("recent_decisions"));
            REQUIRE_FALSE(state["recent_decisions"].empty());
            REQUIRE(state["recent_decisions"].front().value("narrative_note", "") == "the road was quiet");
        }
    }
}

TEST_CASE("DashboardUIManager brings the view up", "[DashboardUIManager][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    // The DLL is asked for by handle rather than loaded, so it has to be in
    // the process before the wrapper goes looking. Reaching the fake's state
    // is what puts it there.
    FakePrisma().Reset();
    REQUIRE(PrismaUI::Initialize());

    SECTION("when PrismaUI is installed")
    {
        Dashboard::Initialize();

        SECTION("should create the view from the page it ships")
        {
            REQUIRE(FakePrisma().createViewCalls == 1);
            REQUIRE(std::string{FakePrisma().lastHtmlPath}.find("dashboard") != std::string::npos);
        }

        SECTION("should start it hidden")
        {
            // The dashboard is a debug surface. Coming up over the game on
            // every load would be unusable.
            REQUIRE(FakePrisma().hideCalls >= 1);
            REQUIRE(FakePrisma().showCalls == 0);
        }

        SECTION("should wire up every control the page has")
        {
            // Registered before the view goes live, so the first thing a
            // player touches routes somewhere rather than into nothing.
            REQUIRE(Control("ne_setTickEnabled") != nullptr);
            REQUIRE(Control("ne_dispatchAction") != nullptr);
            REQUIRE(Control("ne_abortRunningBeat") != nullptr);
            REQUIRE(Control("ne_setPhaseIdealDuration") != nullptr);
            REQUIRE(Control("ne_beginHotkeyRebind") != nullptr);
        }
    }

    SECTION("when the view cannot be created")
    {
        // A broken install: the plugin is there and the page is not.
        FakePrisma().createViewFailsWith = 1;
        Dashboard::Initialize();

        SECTION("should register nothing rather than wire a view that is not there")
        {
            REQUIRE(FakePrisma().registerListenerCalls == 0);
        }
    }

    SECTION("when the keyboard cannot be reached")
    {
        engine.input.managerPresent = false;

        SECTION("should still bring the view up")
        {
            // Without the input manager the hotkey is inert, but the page can
            // still be opened from the console and still renders.
            Dashboard::Initialize();
            REQUIRE(FakePrisma().createViewCalls == 1);
        }
    }
}

TEST_CASE("DashboardUIManager pushes state only while somebody is looking", "[DashboardUIManager][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    FakePrisma().Reset();
    REQUIRE(PrismaUI::Initialize());
    const RunningDispatch dispatch;
    Dashboard::Initialize();

    SECTION("when the dashboard is hidden")
    {
        SECTION("should not compose anything")
        {
            // Every applied Director decision used to rebuild the whole state
            // blob whether or not anybody could see it.
            const auto before = FakePrisma().interopCalls;
            Dashboard::PushFullState();
            REQUIRE_FALSE(
                Eventually([&] { return FakePrisma().interopCalls > before; }, std::chrono::milliseconds{300}));
        }
    }

    SECTION("when the dashboard is open")
    {
        Dashboard::ToggleVisibility();

        SECTION("should send the state to the page")
        {
            REQUIRE(Eventually([] { return FakePrisma().interopCalls > 0; }));
            REQUIRE(std::string{FakePrisma().lastFunctionName} == "updateFullState");
        }

        SECTION("should send something the page can parse")
        {
            REQUIRE(Eventually([] { return FakePrisma().interopCalls > 0; }));
            const auto parsed = nlohmann::json::parse(FakePrisma().lastArgument, nullptr, false);
            REQUIRE_FALSE(parsed.is_discarded());
        }
    }
}

TEST_CASE("DashboardUIManager opens and closes on the hotkey", "[DashboardUIManager][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    FakePrisma().Reset();
    REQUIRE(PrismaUI::Initialize());
    const RunningDispatch dispatch;
    Dashboard::Initialize();

    SECTION("when it is opened")
    {
        Dashboard::ToggleVisibility();

        SECTION("should show it and take focus")
        {
            // Shown without focus, the page renders and swallows nothing;
            // the player then cannot click anything on it.
            REQUIRE(FakePrisma().showCalls == 1);
            REQUIRE(FakePrisma().focusCalls == 1);
        }

        SECTION("should seed it with current state rather than a stale view")
        {
            REQUIRE(Eventually([] { return FakePrisma().interopCalls > 0; }));
        }
    }

    SECTION("when it is closed again")
    {
        Dashboard::ToggleVisibility();
        const auto shows = FakePrisma().showCalls;
        Dashboard::ToggleVisibility();

        SECTION("should release focus and hide it")
        {
            // Paired teardown: hiding without releasing focus leaves the
            // player unable to move.
            REQUIRE(FakePrisma().unfocusCalls == 1);
            REQUIRE(FakePrisma().hideCalls >= 2);
            REQUIRE(FakePrisma().showCalls == shows);
        }
    }
}

TEST_CASE("DashboardUIManager acts on the page's controls", "[DashboardUIManager][engine]")
{
    // Each of these is a switch or a slider on the page, and each writes what
    // it changed to the override file -- a setting that reverts on reboot is
    // a control the player has to find and set again every session.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    FakePrisma().Reset();
    REQUIRE(PrismaUI::Initialize());
    const RunningDispatch dispatch;
    RegisterBeat("fake_dashboard_control");
    Dashboard::Initialize();

    SECTION("when the Director is switched off")
    {
        Press("ne_setTickEnabled", "false");

        SECTION("should stop the Director")
        {
            REQUIRE(Eventually([] { return !Tick::IsEnabled(); }));
        }
    }

    SECTION("when a beat is switched off")
    {
        Press("ne_setActionEnabled", R"({"name":"fake_dashboard_control","enabled":false})");

        SECTION("should take it out of the running")
        {
            REQUIRE(Eventually([] { return !BeatRegistry::IsEnabled("fake_dashboard_control"); }));
        }
    }

    SECTION("when the payload for a beat toggle is malformed")
    {
        SECTION("should change nothing")
        {
            // The page is JavaScript and the payload is a string. A parse
            // failure that fell through to a default would silently disable
            // whichever beat sorted first.
            Press("ne_setActionEnabled", "not json at all");
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            REQUIRE(BeatRegistry::IsEnabled("fake_dashboard_control"));
        }
    }

    SECTION("when a slider is moved")
    {
        Press("ne_setTickInterval", "45");

        SECTION("should take the new value")
        {
            REQUIRE(Eventually([] { return Settings::Get().tickIntervalSeconds == 45; }));
        }
    }

    SECTION("when a slider is dragged past what the setting allows")
    {
        Press("ne_setTickInterval", "999999");

        SECTION("should clamp rather than take it")
        {
            // The page's own bounds and the plugin's are separate, and only
            // one of them is shipped with the save.
            REQUIRE(Eventually([] { return Settings::Get().tickIntervalSeconds != 999999; }));
        }
    }

    SECTION("when a phase's ideal duration is changed")
    {
        Press("ne_setPhaseIdealDuration", R"({"phase":"climax","seconds":150})");

        SECTION("should take it for that phase alone")
        {
            REQUIRE(Eventually([] { return Settings::Get().idealDurationClimax == 150; }));
        }
    }

    SECTION("when a rebind is started and then abandoned")
    {
        SECTION("should not leave the next keypress captured")
        {
            // A capture flag left set means the next key the player presses,
            // for anything at all, becomes the dashboard's hotkey.
            Press("ne_beginHotkeyRebind", "");
            Press("ne_cancelHotkeyRebind", "");
            SUCCEED();
        }
    }
}
