#include <BeatSystem.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>

#include <BeatRegistry.h>
#include <DecisionLog.h>
#include <EvalDispatch.h>
#include <IBeat.h>
#include <MainThread.h>
#include <PhaseTracker.h>
#include <PluginThread.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <Snapshot.h>

#include <nlohmann/json.hpp>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

// Tests for the thing that decides whether a beat happens at all.
//
// Between the Director's evaluation and any beat firing there is a walk of
// gates, and every one of them exists because a beat that fires at the wrong
// moment is worse than no beat: one arriving on top of another, one arriving
// while the phase has only just turned over, the same beat twice running. The
// gates are silent in the game -- the symptom is a mod that seems to do
// nothing -- so what these cases pin is which gate stops what.
//
// Past the gates it is a model call, and the answer is a beat's name. Whether
// that name is one of the candidates offered is the only thing standing
// between a hallucinated action and a beat that never existed being started.
//
// Mocked-engine, because the gate walk reads the world. The beats are fakes:
// what the system does with a beat is start it, tick it and abort it, and a
// fake records all three. The registry has no way to unregister, so every case
// registers under names of its own and the fakes decline outside the case that
// made them -- the same discipline the registry's own tests use.

namespace
{
    namespace BeatSystem = NarrativeEngine::BeatSystem;
    namespace BeatRegistry = NarrativeEngine::BeatRegistry;
    namespace DecisionLog = NarrativeEngine::DecisionLog;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::BeatPolarity;
    using NarrativeEngine::BeatState;
    using NarrativeEngine::IBeat;
    using NarrativeEngine::Snapshot;
    using NarrativeEngine::TickMode;
    using NarrativeEngine::TickResult;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    // No dwell to serve and no cooldown to wait out, so a case that is not
    // about a gate never has to satisfy one.
    constexpr const char* kSettings = "[General]\nbDebugMode=0\n"
                                      "[BeatSystem]\niBeatCooldownSeconds=0\niBeatSystemPollIntervalMs=50\n"
                                      "[Director]\niIdealDurationExposition=0\niIdealDurationRisingAction=0\n"
                                      "iIdealDurationClimax=0\niIdealDurationFallingAction=0\n"
                                      "iIdealDurationResolution=0\n";

    // Which case is running. A fake registered by an earlier case sees a
    // different value and declines, so the candidate list stays attributable.
    std::atomic<int> g_activeCase{0};

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

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    // What the picker answers with. `action` names the beat.
    std::string Answer(const std::string& beatName, const std::string& parameters = "{}")
    {
        return R"({"action":")" + beatName + R"(","parameters":)" + parameters
               + R"(,"narrative_note":"because the road was quiet"})";
    }

    class FakeBeat : public IBeat
    {
    public:
        FakeBeat(std::string name, bool available) : name_(std::move(name)), available_(available) {}

        std::string Name() const override
        {
            return name_;
        }
        std::string Description() const override
        {
            return "A fake beat, for the beat system's benefit.";
        }
        BeatPolarity Polarity() const override
        {
            return BeatPolarity::Either;
        }

        bool IsAvailable(const BeatContext&) const override
        {
            return available_ && owningCase_ == g_activeCase.load();
        }

        void OnStart(const BeatContext&, const nlohmann::json& parameters) override
        {
            ++starts;
            lastParameters = parameters.dump();
        }

        TickResult Tick(const PluginThread::Token&, TickMode, BeatState state) override
        {
            ++ticks;
            lastTickedState = state;
            return finishOnTick ? TickResult{BeatState::NOT_RUNNING} : TickResult{};
        }

        void Abort(const NarrativeEngine::MainThread::Token&) override
        {
            ++aborts;
        }

        // Re-arm a registration that survived an earlier leaf, so every leaf
        // sees a beat that answers for it and counters at zero.
        void Adopt(int owningCase, bool available)
        {
            owningCase_ = owningCase;
            available_ = available;
            starts = 0;
            ticks = 0;
            aborts = 0;
            finishOnTick = false;
            lastParameters.clear();
            lastTickedState = BeatState::NOT_RUNNING;
        }

        std::atomic<int> starts{0};
        std::atomic<int> ticks{0};
        std::atomic<int> aborts{0};
        std::atomic<bool> finishOnTick{false};
        std::string lastParameters;
        BeatState lastTickedState = BeatState::NOT_RUNNING;

    private:
        std::string name_;
        bool available_ = true;
        int owningCase_ = g_activeCase.load();
    };

    // Registers a beat and hands back whatever the registry now holds under
    // that name. Looked up rather than borrowed: top-level setup re-runs per
    // leaf, so from the second leaf on the registration is a rejected
    // duplicate and the object just built is destroyed on the way out.
    FakeBeat* RegisterBeat(const std::string& name, bool available = true)
    {
        BeatRegistry::Register(std::make_unique<FakeBeat>(name, available));
        auto* registered = static_cast<FakeBeat*>(BeatRegistry::Find(name));
        REQUIRE(registered != nullptr);
        registered->Adopt(g_activeCase.load(), available);
        BeatRegistry::SetEnabled(name, NarrativeEngine::Settings::GetBeatEnabled(name, true));
        return registered;
    }

    // A world that has been in its phase long enough for the dwell gate, with
    // a tension score the director already settled on.
    Snapshot SettledSnapshot()
    {
        Snapshot s;
        s.realTimeSec = 1234.0;
        s.currentPhase = PhaseTracker::PhaseName(PhaseTracker::Phase::RisingAction);
        s.timeInPhaseSeconds = 600.0f;
        return s;
    }

    DecisionLog::DecisionRecord PlainRecord()
    {
        DecisionLog::DecisionRecord rec;
        rec.tensionScore = 40;
        return rec;
    }

    // Run one consideration the way the evaluation does, and say whether the
    // system finished with it.
    bool Consider(Snapshot snapshot, DecisionLog::DecisionRecord rec)
    {
        bool finalized = false;
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            BeatSystem::ConsiderBeat(pt, std::move(snapshot), std::move(rec), [&] { finalized = true; });
        });
        return finalized;
    }

    void StartBeatDirectly(const std::string& name, const nlohmann::json& parameters = nlohmann::json::object())
    {
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { BeatSystem::StartBeat(pt, name, parameters); });
    }

    bool AbortFromMainThread()
    {
        bool aborted = false;
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            NarrativeEngine::MainThread::Run(
                pt, [&](const NarrativeEngine::MainThread::Token& mt) { aborted = BeatSystem::AbortRunningBeat(mt); });
        });
        return aborted;
    }

    // Wait for something a worker will do. Bounded, and asserted on by the
    // caller, so a system that stops answering fails rather than hangs.
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

    struct RunningEvalDispatch
    {
        RunningEvalDispatch()
        {
            NarrativeEngine::EvalDispatch::Start();
        }
        ~RunningEvalDispatch()
        {
            NarrativeEngine::EvalDispatch::Stop();
        }
        RunningEvalDispatch(const RunningEvalDispatch&) = delete;
        RunningEvalDispatch& operator=(const RunningEvalDispatch&) = delete;
    };

    std::string SelectedBeat()
    {
        const auto tail = DecisionLog::Tail(1);
        return tail.empty() ? std::string{} : tail.front().beatSelected;
    }
} // namespace

TEST_CASE("BeatSystem says what is running", "[BeatSystem][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    g_activeCase = 1;
    BeatSystem::OnRevert();
    auto* beat = RegisterBeat("fake_says_running");

    SECTION("when nothing is in flight")
    {
        SECTION("should name nobody")
        {
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::NO_BEAT_RUNNING);
            REQUIRE(BeatSystem::GetRunningBeatName().empty());
            REQUIRE_FALSE(BeatSystem::GetInFlightInfo().has_value());
        }
    }

    SECTION("when a beat has been started")
    {
        StartBeatDirectly("fake_says_running");

        SECTION("should name it")
        {
            // The dashboard reads this, and so does the gate that stops a
            // second beat arriving on top of the first.
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::BEAT_RUNNING);
            REQUIRE(BeatSystem::GetRunningBeatName() == "fake_says_running");
        }

        SECTION("should say what it is doing and since when")
        {
            const auto info = BeatSystem::GetInFlightInfo();
            REQUIRE(info.has_value());
            REQUIRE(info->name == "fake_says_running");
            REQUIRE(info->state == BeatState::COMPOSE);
            REQUIRE(info->startedAtRealSeconds > 0.0);
        }

        SECTION("should have told the beat to begin")
        {
            REQUIRE(beat->starts.load() == 1);
        }
    }
}

TEST_CASE("BeatSystem starts a beat when asked directly", "[BeatSystem][engine]")
{
    // The entry point the co-save restore and the debug console both use.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    g_activeCase = 2;
    BeatSystem::OnRevert();
    auto* first = RegisterBeat("fake_direct_first");
    auto* second = RegisterBeat("fake_direct_second");

    SECTION("when the beat is registered")
    {
        nlohmann::json parameters;
        parameters["sender_npc_form_id"] = "0x1A6A0";
        StartBeatDirectly("fake_direct_first", parameters);

        SECTION("should hand it the parameters it was given")
        {
            // The beat parses its own parameters and fails to start without
            // them, so dropping them here is a beat that never fires and
            // never says why.
            REQUIRE(first->lastParameters.find("0x1A6A0") != std::string::npos);
        }
    }

    SECTION("when one is already running")
    {
        StartBeatDirectly("fake_direct_first");

        SECTION("should refuse the second")
        {
            // Two beats at once is two narrations, two quests and two sets of
            // world-side effects unwinding over each other.
            StartBeatDirectly("fake_direct_second");
            REQUIRE(second->starts.load() == 0);
            REQUIRE(BeatSystem::GetRunningBeatName() == "fake_direct_first");
        }
    }

    SECTION("when the name is not one the registry knows")
    {
        SECTION("should leave the system idle")
        {
            StartBeatDirectly("fake_direct_nonexistent");
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::NO_BEAT_RUNNING);
        }
    }
}

TEST_CASE("BeatSystem refuses to consider a beat when the world says no", "[BeatSystem][engine]")
{
    // The gate walk. Every one of these files a decision and hands control
    // back: a gate that failed to finalise would leave the evaluation latched
    // and silence the Director for the rest of the session.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    g_activeCase = 3;
    BeatSystem::OnRevert();
    DecisionLog::Clear();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    Say(FakeLLM().promptResponse, Answer("fake_gates"));
    auto* beat = RegisterBeat("fake_gates");

    SECTION("when a beat is already in flight")
    {
        StartBeatDirectly("fake_gates");

        SECTION("should not ask the model at all")
        {
            const auto prompts = FakeLLM().sendPromptCalls;
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(FakeLLM().sendPromptCalls == prompts);
            REQUIRE(SelectedBeat().empty());
        }
    }

    SECTION("when the phase turned over this very tick")
    {
        auto rec = PlainRecord();
        rec.advancedToPhase = PhaseTracker::Phase::Climax;

        SECTION("should let the new phase settle first")
        {
            // A beat fired on the tick the phase changed is a beat chosen
            // against the phase it just left.
            REQUIRE(Consider(SettledSnapshot(), rec));
            REQUIRE(SelectedBeat().empty());
        }
    }

    SECTION("when the last beat was too recent")
    {
        const ConfiguredSettings patient{"[BeatSystem]\niBeatCooldownSeconds=120\n"};

        SECTION("should wait out the cooldown")
        {
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(SelectedBeat().empty());
        }
    }

    SECTION("when the phase has barely begun")
    {
        const ConfiguredSettings unhurried{"[Director]\niIdealDurationRisingAction=600\n"};
        auto snapshot = SettledSnapshot();
        snapshot.timeInPhaseSeconds = 5.0f;

        SECTION("should let it run its length first")
        {
            REQUIRE(Consider(snapshot, PlainRecord()));
            REQUIRE(SelectedBeat().empty());
        }
    }

    SECTION("when the phase is one it does not recognise")
    {
        auto snapshot = SettledSnapshot();
        snapshot.currentPhase = "Interlude";

        SECTION("should decline rather than guess")
        {
            // A phase name it cannot place has no ideal duration to measure
            // dwell against, so there is nothing to decide with.
            REQUIRE(Consider(snapshot, PlainRecord()));
            REQUIRE(SelectedBeat().empty());
        }
    }

    SECTION("when the player is fighting")
    {
        engine.player.inCombat = true;

        SECTION("should stay out of it")
        {
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(SelectedBeat().empty());
        }
    }

    SECTION("when nothing on the roster is available")
    {
        beat->Adopt(g_activeCase.load(), /*available=*/false);

        SECTION("should not ask the model which of nothing to pick")
        {
            const auto prompts = FakeLLM().sendPromptCalls;
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(FakeLLM().sendPromptCalls == prompts);
            REQUIRE(SelectedBeat().empty());
        }
    }
}

TEST_CASE("BeatSystem picks a beat once the gates are clear", "[BeatSystem][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    g_activeCase = 4;
    BeatSystem::OnRevert();
    DecisionLog::Clear();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    auto* beat = RegisterBeat("fake_picked");
    // Registered and unavailable, so it is a name the registry knows and the
    // candidate list does not offer -- which is the only way to tell the
    // candidate check apart from the registry lookup behind it.
    auto* offstage = RegisterBeat("fake_not_offered", /*available=*/false);

    SECTION("when the model names one of the candidates")
    {
        Say(FakeLLM().promptResponse, Answer("fake_picked", R"({"urgency_hint":"high"})"));
        REQUIRE(Consider(SettledSnapshot(), PlainRecord()));

        SECTION("should start it")
        {
            REQUIRE(beat->starts.load() == 1);
            REQUIRE(BeatSystem::GetRunningBeatName() == "fake_picked");
        }

        SECTION("should pass the parameters the model chose")
        {
            REQUIRE(beat->lastParameters.find("high") != std::string::npos);
        }

        SECTION("should file what it decided and why")
        {
            // The decision log is the only account of what the Director did
            // and the only thing a player diagnosing a quiet session can read.
            const auto tail = DecisionLog::Tail(1);
            REQUIRE(tail.size() == 1);
            REQUIRE(tail.front().beatSelected == "fake_picked");
            REQUIRE(tail.front().narrativeNote == "because the road was quiet");
        }
    }

    SECTION("when the model names a beat that was not offered")
    {
        // Either a hallucination or a beat that dropped out of the candidate
        // list while the model was thinking. Starting it would run a beat the
        // gates had already refused.
        Say(FakeLLM().promptResponse, Answer("fake_not_offered"));

        SECTION("should start nothing")
        {
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(offstage->starts.load() == 0);
            REQUIRE(beat->starts.load() == 0);
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::NO_BEAT_RUNNING);
        }
    }

    SECTION("when the model declines to pick anything")
    {
        Say(FakeLLM().promptResponse, R"({"action":"","narrative_note":"nothing fits"})");

        SECTION("should start nothing and still finish")
        {
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(beat->starts.load() == 0);
        }
    }

    SECTION("when the model answers with something that is not a decision")
    {
        Say(FakeLLM().promptResponse, "not json at all");

        SECTION("should start nothing and still finish")
        {
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(beat->starts.load() == 0);
        }
    }

    SECTION("when the model call fails outright")
    {
        FakeLLM().sendPromptSucceeds = false;

        SECTION("should start nothing and still finish")
        {
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(beat->starts.load() == 0);
        }
    }
}

TEST_CASE("BeatSystem does not offer the same beat twice running", "[BeatSystem][engine]")
{
    // The repetition window. One beat firing over and over reads as a broken
    // mod long before it reads as a theme.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    g_activeCase = 5;
    BeatSystem::OnRevert();
    DecisionLog::Clear();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    auto* beat = RegisterBeat("fake_repeat");
    Say(FakeLLM().promptResponse, Answer("fake_repeat"));
    REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
    REQUIRE(beat->starts.load() == 1);
    REQUIRE(AbortFromMainThread());

    SECTION("when it fired a moment ago")
    {
        SECTION("should leave it out of the candidates")
        {
            // The candidate list is then empty, so the model is not asked at
            // all -- which is the saving as well as the point.
            const auto prompts = FakeLLM().sendPromptCalls;
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(FakeLLM().sendPromptCalls == prompts);
            REQUIRE(beat->starts.load() == 1);
        }
    }

    SECTION("when the window has passed")
    {
        const ConfiguredSettings forgetful{"[BeatSystem]\niBeatCooldownSeconds=0\n"
                                           "iBeatRepetitionWindowSeconds=0\n"};

        SECTION("should offer it again")
        {
            REQUIRE(Consider(SettledSnapshot(), PlainRecord()));
            REQUIRE(beat->starts.load() == 2);
        }
    }
}

TEST_CASE("BeatSystem force-dispatches for the dashboard", "[BeatSystem][engine]")
{
    // The debug button. It bypasses every gate except the one that stops two
    // beats running at once, and still goes through the model so parameter
    // validation takes the same path a real dispatch would.
    EngineMock engine;
    const ConfiguredSettings settings{"[General]\nbDebugMode=0\n"
                                      "[BeatSystem]\niBeatCooldownSeconds=600\n"
                                      "[Director]\niIdealDurationRisingAction=600\n"};
    g_activeCase = 6;
    BeatSystem::OnRevert();
    DecisionLog::Clear();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    auto* beat = RegisterBeat("fake_forced");
    Say(FakeLLM().promptResponse, Answer("fake_forced"));
    // Force-dispatch hands the model round trip to the evaluation worker
    // rather than running it inline, so the worker has to be up.
    const RunningEvalDispatch worker;

    SECTION("when the gates would have refused")
    {
        SECTION("should dispatch anyway")
        {
            // A cooldown of ten minutes and a dwell of ten minutes, neither
            // satisfied. The whole use of the button is not waiting. The
            // model round trip is handed to the evaluation worker rather than
            // run inline, so the button returns before the beat starts.
            BeatSystem::ForceDispatchBeat("fake_forced");
            REQUIRE(Eventually([&] { return beat->starts.load() == 1; }));
        }
    }

    SECTION("when a beat is already running")
    {
        StartBeatDirectly("fake_forced");

        SECTION("should refuse")
        {
            // The one gate it never bypasses.
            const auto starts = beat->starts.load();
            BeatSystem::ForceDispatchBeat("fake_forced");
            REQUIRE(beat->starts.load() == starts);
        }
    }

    SECTION("when the name is not one the registry knows")
    {
        SECTION("should leave the system idle")
        {
            BeatSystem::ForceDispatchBeat("fake_forced_nonexistent");
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::NO_BEAT_RUNNING);
        }
    }
}

TEST_CASE("BeatSystem takes a running beat away", "[BeatSystem][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    g_activeCase = 7;
    BeatSystem::OnRevert();
    auto* beat = RegisterBeat("fake_aborted");

    SECTION("when one is running")
    {
        StartBeatDirectly("fake_aborted");
        const bool aborted = AbortFromMainThread();

        SECTION("should tell the beat to unwind itself")
        {
            // The beat owns its own world-side effects -- a spawned actor, a
            // started quest, a promoted faction rank -- and nothing else can
            // put them back.
            REQUIRE(aborted);
            REQUIRE(beat->aborts.load() == 1);
        }

        SECTION("should let another be considered straight away")
        {
            // The cooldown is cleared too: an aborted beat did not happen, so
            // charging the player a quiet window for it is wrong twice over.
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::NO_BEAT_RUNNING);
            REQUIRE(BeatSystem::GetRunningBeatName().empty());
        }
    }

    SECTION("when nothing is running")
    {
        SECTION("should say so rather than pretend")
        {
            REQUIRE_FALSE(AbortFromMainThread());
            REQUIRE(beat->aborts.load() == 0);
        }
    }
}

TEST_CASE("BeatSystem carries the running beat across a save", "[BeatSystem][engine]")
{
    // What a beat mid-flight looks like from a load. Getting this wrong either
    // strands the system in BEAT_RUNNING with nothing behind it -- which no
    // other path recovers from -- or drops a beat that is still out there.
    //
    // One branch here is out of reach: a save naming a beat this build no
    // longer has, which the loader recovers from by resetting to idle. The
    // registry has no way to unregister and refuses to start a name it does
    // not hold, so there is no way to write such a save from inside a test.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    g_activeCase = 8;
    BeatSystem::OnRevert();
    RegisterBeat("fake_saved");
    StartBeatDirectly("fake_saved");

    SECTION("when the state is written and read back")
    {
        BeatSystem::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        BeatSystem::OnRevert();
        BeatSystem::OnLoad(FakeInterface(), 1, 0);

        SECTION("should still know which beat is out there")
        {
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::BEAT_RUNNING);
            REQUIRE(BeatSystem::GetRunningBeatName() == "fake_saved");
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        BeatSystem::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        BeatSystem::OnRevert();
        BeatSystem::OnLoad(FakeInterface(), 99, 0);

        SECTION("should come back idle")
        {
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::NO_BEAT_RUNNING);
        }
    }

    SECTION("when the record ends early")
    {
        engine.cosave.readable.clear();
        engine.cosave.readCursor = 0;
        BeatSystem::OnRevert();

        SECTION("should come back idle")
        {
            BeatSystem::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(BeatSystem::GetTopLevelState() == BeatSystem::TopLevelState::NO_BEAT_RUNNING);
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should write nothing")
        {
            engine.cosave.written.clear();
            BeatSystem::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
        }

        SECTION("should leave what is live alone")
        {
            BeatSystem::OnLoad(nullptr, 1, 0);
            REQUIRE(BeatSystem::GetRunningBeatName() == "fake_saved");
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.written.clear();
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            BeatSystem::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}
