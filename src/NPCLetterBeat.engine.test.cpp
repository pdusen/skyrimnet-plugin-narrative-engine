#include <NPCLetterBeat.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <LetterPoolSpies.h>

#include <AsyncDispatch.h>
#include <IBeat.h>
#include <LetterPool.h>
#include <MainThread.h>
#include <PluginThread.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// Tests for the beat that has an NPC write to the player.
//
// The beat's job is a hand-off in three parts: ask the model for a letter, put
// it in one of the letter pool's twenty slots, and then get the vanilla courier
// to carry it. Only the middle part is its own; the first belongs to
// LetterComposer and the last to a Papyrus quest that fills two reference
// aliases at its own pace. So most of what the beat does is WAIT, and most of
// what can go wrong is waiting for something that never arrives.
//
// Mocked-engine, because every step touches the engine: the quests, their
// aliases, the sender's faction rank, and the courier's own container. The
// letter pool itself is stood in for (see LetterPoolSpies.h) -- the beat is
// responsible for asking it for a slot and for giving the slot back, not for
// the pool's own lifecycle.
//
// Two pieces of the module resolve exactly once per process and cannot be
// un-resolved: the per-slot quest cache and the vanilla courier lookup. Cases
// about a world where those are missing are therefore whole TEST_CASEs of their
// own, each building its own deficient world before anything resolves.

namespace
{
    namespace LetterPool = NarrativeEngine::LetterPool;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace Cooldowns = NarrativeEngine::NPCLetterBeat_Cooldowns;
    namespace Init = NarrativeEngine::NPCLetterBeat_Init;
    namespace Persistence = NarrativeEngine::NPCLetterBeat_Persistence;
    namespace QuestControl = NarrativeEngine::NPCLetterBeat_QuestControl;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::BeatPolarity;
    using NarrativeEngine::BeatState;
    using NarrativeEngine::NPCLetterBeat;
    using NarrativeEngine::TickMode;
    using NarrativeEngine::TickResult;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;
    using NarrativeEngine::Testing::LetterPoolSpies;

    // A one-second poll interval keeps the RUNNING arm's verify window eight
    // ticks wide rather than thirty-two; everything else is the shipping
    // default.
    constexpr const char* kSettings = "[General]\nbDebugMode=0\n"
                                      "[BeatSystem]\niBeatSystemPollIntervalMs=1000\n"
                                      "[Director]\niLetterMinSenderCandidates=3\n"
                                      "[Beats]\niLetterBeatCooldownGameHours=24\n"
                                      "iLetterSenderCooldownGameHours=72\n"
                                      "iLetterDispatchVerifyDelaySeconds=8\n"
                                      "iLetterContentMinWords=5\niLetterContentMaxWords=40\n";

    constexpr std::uint32_t kSender = 0x0001A6A0u;
    constexpr std::uint32_t kSenderFaction = 0x0E0000FFu;
    constexpr std::uint32_t kBook = 0x0E000001u;
    constexpr std::uint32_t kLetterRef = 0x0E00A001u;
    constexpr std::uint32_t kPooledQuestBase = 0x0E001000u;

    // The version the beat stamps its co-save record with. A record carrying
    // anything else is discarded, so a test reading its own save back has to
    // name the current one.
    constexpr std::uint32_t kRecordVersion = 2;

    FakeSkyrimNetState& FakeLLM()
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

    std::string EngagementRows(int count)
    {
        std::string rows = "[";
        for (int i = 0; i < count; ++i) {
            if (i > 0)
                rows += ",";
            rows += R"({"formId":)" + std::to_string(kSender + static_cast<std::uint32_t>(i))
                    + R"(,"name":"Somebody","totalMemoryImportance":12.5,"lastEventTime":100.0})";
        }
        return rows + "]";
    }

    std::string LetterAnswer()
    {
        nlohmann::json j;
        j["letter_label"] = "A note";
        j["body"] = "I have been turning this over since you left and I mean to put it in writing.";
        j["mood"] = "warm";
        j["topic_tag"] = "the tusk";
        j["tags"] = nlohmann::json::array({"trade", "debt"});
        return j.dump();
    }

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    // The twenty delivery quests the beat resolves at data load, each with the
    // two reference aliases it waits on.
    std::vector<EngineMock::AliasedQuest> BuildPooledQuests(EngineMock& engine, std::size_t count, bool withAliases)
    {
        std::vector<EngineMock::AliasedQuest> built;
        for (std::size_t i = 0; i < count; ++i) {
            char editorID[64];
            std::snprintf(editorID, sizeof(editorID), "_ne_PooledLetterQuest%02zu", i);
            EngineMock::QuestState state;
            state.editorID = editorID;
            state.formID = kPooledQuestBase + static_cast<std::uint32_t>(i);
            state.running = false;
            const std::vector<std::string> aliases =
                withAliases ? std::vector<std::string>{"Sender", "LetterRef"} : std::vector<std::string>{};
            built.push_back(engine.AddQuestWithAliases(state, aliases));
        }
        return built;
    }

    // The whole world the beat needs to get a letter out: the vanilla courier,
    // twenty delivery quests, the faction the Sender alias fills from, the NPC
    // doing the writing, and the Book form the letter is written into.
    std::vector<EngineMock::AliasedQuest> BuildWorld(EngineMock& engine)
    {
        engine.AddCourierQuest(/*withContainerAlias=*/true, /*withContainerRef=*/true);
        auto quests = BuildPooledQuests(engine, LetterPool::kPoolSize, /*withAliases=*/true);
        engine.AddFaction(kSenderFaction, "_ne_LetterSenderFaction");
        engine.AddActor(kSender);
        engine.AddBook(kBook);
        // A letter comes from somebody the player is NOT standing next to, so
        // the composer refuses a sender who is loaded in the world or shares
        // the player's location hub. Put them out of sight and out of reach.
        engine.visibility.target3DPresent = false;
        engine.world.playerHasLocation = false;
        return quests;
    }

    void PrimeModel(int candidates = 3)
    {
        auto& fake = FakeLLM();
        fake.Reset();
        fake.memorySystemReady = true;
        SetJson(fake.engagementJson, EngagementRows(candidates));
        SetJson(fake.memoriesJson,
                R"([{"type":"EXPERIENCE","content":"She spoke of the mammoth tusk.","importance_score":0.8,)"
                R"("game_time":50.0,"emotion":"calm","location":"Whiterun"}])");
        SetJson(fake.promptResponse, LetterAnswer());
    }

    nlohmann::json SenderParams(const char* id = "0x1A6A0")
    {
        nlohmann::json params;
        params["sender_npc_form_id"] = id;
        return params;
    }

    TickResult Tick(NPCLetterBeat& beat, BeatState state, TickMode mode = TickMode::Normal)
    {
        TickResult result;
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { result = beat.Tick(pt, mode, state); });
        return result;
    }

    // Drive one arm of the machine until it asks to leave. The compose answer
    // comes back on the dispatch thread, so ticking is the only way to learn
    // that it has; the arm's own timeouts bound the loop.
    BeatState RunArm(NPCLetterBeat& beat, BeatState state, int maxTicks = 400)
    {
        for (int i = 0; i < maxTicks; ++i) {
            if (const auto result = Tick(beat, state); result.transitionTo)
                return *result.transitionTo;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return state;
    }

    // Take the beat all the way through one successful dispatch. The
    // per-beat cooldown is stamped on no other path, so anything about the
    // cooldown has to go the whole way round first.
    void SendOneLetter(NPCLetterBeat& beat, EngineMock& engine, const std::vector<EngineMock::AliasedQuest>& quests)
    {
        auto* letter = engine.AddReference(nullptr, kLetterRef, {});
        engine.FillRefAlias(quests.front().aliases.at(0), letter);
        engine.FillRefAlias(quests.front().aliases.at(1), letter);
        beat.OnStart(BeatContext{}, SenderParams());
        REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::RUNNING);
        REQUIRE(RunArm(beat, BeatState::RUNNING) == BeatState::CLEANUP);
        REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);
    }

    // Write the ledgers out and read them straight back, as loading a save
    // that was made a moment ago does.
    void SaveAndReload(EngineMock& engine, std::uint32_t version = kRecordVersion)
    {
        Persistence::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        Persistence::OnRevert();
        Persistence::OnLoad(FakeInterface(), version, 0);
    }

    // The Director taking the beat away mid-run. Reached the way production
    // reaches it: a plugin-thread caller hopping to the main thread, which is
    // the only place a main-thread token exists.
    void AbortFromMainThread(NPCLetterBeat& beat)
    {
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            NarrativeEngine::MainThread::Run(pt, [&](const NarrativeEngine::MainThread::Token& mt) { beat.Abort(mt); });
        });
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

TEST_CASE("NPCLetterBeat says what it is", "[NPCLetterBeat][engine]")
{
    const NPCLetterBeat beat;

    SECTION("should answer to the name the Director picks it by")
    {
        // The name is the value the beat-select model returns, the key its
        // cosave record is filed under, and the tag every log line carries.
        REQUIRE(beat.Name() == "npc_letter");
    }

    SECTION("should serve either direction")
    {
        // A letter can be a thank-you or a threat, so the Director may reach
        // for it whichever way it wants tension to move.
        REQUIRE(beat.Polarity() == BeatPolarity::Either);
    }

    SECTION("should tell the model which parameter it must supply")
    {
        // The description IS the prompt the picker reads. A sender it does not
        // ask for is a beat that fails to start every time it is chosen.
        REQUIRE(beat.Description().find("sender_npc_form_id") != std::string::npos);
    }
}

TEST_CASE("NPCLetterBeat says when it can be offered", "[NPCLetterBeat][engine]")
{
    // Happy path, re-run per leaf: a courier to carry the letter, a memory
    // system with enough people in it to pick from, and nothing sent yet.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;
    BeatContext ctx;

    SECTION("when nothing is in the way")
    {
        SECTION("should offer itself")
        {
            REQUIRE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when a letter went out recently")
    {
        engine.calendar.hoursPassed = 100.0f;
        SendOneLetter(beat, engine, quests);

        SECTION("should hold off until the window is up")
        {
            // Twenty-three of the twenty-four hours gone is still inside it.
            // Letters arriving back to back is the failure this exists for.
            engine.calendar.hoursPassed = 123.0f;
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }

        SECTION("should offer itself again once it is")
        {
            engine.calendar.hoursPassed = 125.0f;
            REQUIRE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the player is somewhere a courier could not reach")
    {
        auto* den = engine.AddLocation(0x0E00B001u, "Bleak Falls Barrow", {"LocTypeBanditCamp"});
        engine.world.playerHasLocation = true;
        engine.world.playerLocationOverride = den;
        ctx.player = engine.AddActor(0x00000014u);

        SECTION("should stay out of it")
        {
            // The courier cannot walk into a barrow, and a letter arriving
            // mid-dungeon reads as a bug whether or not one could.
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the memory system is not up")
    {
        FakeLLM().memorySystemReady = false;

        SECTION("should stay out of it")
        {
            // Without memories there is nothing for a sender to write about,
            // and the composer would spend a model call to find that out.
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the engagement query answers with something that is not a list")
    {
        SetJson(FakeLLM().engagementJson, R"({"error":"no"})");

        SECTION("should stay out of it")
        {
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when too few people are engaged to pick from")
    {
        SetJson(FakeLLM().engagementJson, EngagementRows(2));

        SECTION("should stay out of it")
        {
            // Offering the beat with one plausible sender means the Director
            // picks the same person every time it reaches for a letter.
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }
}

TEST_CASE("NPCLetterBeat counts down its own cooldown", "[NPCLetterBeat][engine]")
{
    // What the dashboard prints beside the beat, and what the Director's own
    // manifest reports when the beat is unavailable.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;

    SECTION("when no letter has ever gone out")
    {
        engine.calendar.hoursPassed = 100.0f;

        SECTION("should report nothing left to wait for")
        {
            // A fresh save, where the stamp is zero rather than long ago.
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }
    }

    SECTION("when one has just gone out")
    {
        engine.calendar.hoursPassed = 100.0f;
        SendOneLetter(beat, engine, quests);

        SECTION("should report the whole window")
        {
            REQUIRE(beat.RemainingCooldownGameHours() == 24.0);
        }

        SECTION("should count it down as the hours pass")
        {
            engine.calendar.hoursPassed = 110.0f;
            REQUIRE(beat.RemainingCooldownGameHours() == 14.0);
        }

        SECTION("should report nothing once the window is out")
        {
            // Clamped rather than negative: the caller renders this straight
            // into a line the player reads.
            engine.calendar.hoursPassed = 130.0f;
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }

        SECTION("should report nothing when the cooldown is switched off")
        {
            const ConfiguredSettings uncapped{"[Beats]\niLetterBeatCooldownGameHours=0\n"};
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }
    }
}

TEST_CASE("NPCLetterBeat gets a letter to the courier", "[NPCLetterBeat][engine]")
{
    // Happy path, re-run per leaf: the whole world, a sender the Director
    // named, and a quest that fills both its aliases the moment it starts.
    // Each case takes one of those away.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;
    auto* letter = engine.AddReference(nullptr, kLetterRef, {});
    auto& slot = quests.front();

    SECTION("when the quest fills its aliases")
    {
        engine.FillRefAlias(slot.aliases.at(0), letter);
        engine.FillRefAlias(slot.aliases.at(1), letter);
        beat.OnStart(BeatContext{}, SenderParams());
        const auto next = RunArm(beat, BeatState::COMPOSE);

        SECTION("should go on to watch for the letter")
        {
            REQUIRE(next == BeatState::RUNNING);
        }

        SECTION("should write what the model composed into the slot")
        {
            // The body IS the letter. Everything after this point moves an
            // item around; nothing writes its text again.
            auto& spies = LetterPoolSpies();
            std::scoped_lock lock(spies.mutex);
            REQUIRE(spies.populated.size() == 1);
            REQUIRE(spies.populated.front().senderNpcFormID == kSender);
            REQUIRE(spies.populated.front().body.starts_with("I have been turning this over"));
            REQUIRE(spies.populated.front().mood == "warm");
        }

        SECTION("should start the slot's own delivery quest")
        {
            // Starting it is what makes the Papyrus side fill the aliases,
            // which is what puts a letter in the sender's hands.
            REQUIRE(engine.questControl.started.size() == 1);
            REQUIRE(engine.questControl.started.front() == static_cast<const void*>(slot.quest));
        }

        SECTION("should put the sender at the rank the alias fills from")
        {
            // The Sender alias fills by faction rank rather than by a form
            // the quest names, so the promotion is the only thing that
            // decides who the letter comes from.
            REQUIRE_FALSE(engine.factions.addToFactionCalls.empty());
            const auto& call = engine.factions.addToFactionCalls.back();
            REQUIRE(call.actorFormID == kSender);
            REQUIRE(call.factionFormID == kSenderFaction);
            REQUIRE(call.rank == 4);
        }
    }

    SECTION("when the Sender alias never fills")
    {
        engine.FillRefAlias(slot.aliases.at(1), letter);
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should give up and hand the slot back")
        {
            // A quest that started but never filled its alias leaves the beat
            // waiting forever and one of twenty slots gone with it.
            REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
            REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().aborted == std::vector<std::size_t>{0});
        }
    }

    SECTION("when the LetterRef alias never fills")
    {
        engine.FillRefAlias(slot.aliases.at(0), letter);
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should give up and hand the slot back")
        {
            // Further along than the Sender case and no better: the sender is
            // named but no letter was ever spawned for them to carry.
            REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
            REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().aborted == std::vector<std::size_t>{0});
        }
    }

    SECTION("when the world is not running normally")
    {
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should not begin composing at all")
        {
            // Paused, in combat, or mid-dialogue. A model call fired here
            // lands its answer in a moment the Director never chose.
            for (int i = 0; i < 40; ++i) {
                REQUIRE_FALSE(Tick(beat, BeatState::COMPOSE, TickMode::Paused).transitionTo.has_value());
            }
            REQUIRE(FakeLLM().sendPromptCalls == 0);
        }
    }
}

TEST_CASE("NPCLetterBeat gives up when it cannot dispatch", "[NPCLetterBeat][engine]")
{
    // Every way the dispatch can fail after the model has already written the
    // letter. What each one has to get right is the same: end in CLEANUP, and
    // do not leave a slot of the twenty behind.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;
    auto* letter = engine.AddReference(nullptr, kLetterRef, {});
    engine.FillRefAlias(quests.front().aliases.at(0), letter);
    engine.FillRefAlias(quests.front().aliases.at(1), letter);

    SECTION("when the Director named a sender that will not parse")
    {
        beat.OnStart(BeatContext{}, SenderParams("not a form id"));

        SECTION("should fail on the first tick without asking the model")
        {
            // OnStart seeds the failure rather than reporting it, so the very
            // first tick has to carry it out. A model call here would be spent
            // writing a letter from nobody.
            REQUIRE(Tick(beat, BeatState::COMPOSE).transitionTo == BeatState::CLEANUP);
            REQUIRE(FakeLLM().sendPromptCalls == 0);
        }
    }

    SECTION("when the model refuses to write anything")
    {
        FakeLLM().sendPromptSucceeds = false;
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should fail without taking a slot")
        {
            REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().allocations == 0);
        }
    }

    SECTION("when the pool has no slot to give")
    {
        {
            std::scoped_lock lock(LetterPoolSpies().mutex);
            LetterPoolSpies().allocationFailure = LetterPool::AllocationFailure::EvictionFailed;
        }
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should fail without starting a quest")
        {
            REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
            REQUIRE(engine.questControl.started.empty());
        }
    }

    SECTION("when the sender has died while the letter was being written")
    {
        // The compose round trip takes seconds of wall-clock, which is long
        // enough for the sender to be killed in front of the player. The beat
        // re-checks at dispatch as well, one step later; that narrower window
        // is not reachable here, because the harness answers the model inline
        // and there is no moment between the two checks to kill anybody in.
        engine.forms.actorIsDead = true;
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should fail without taking a slot")
        {
            REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
            REQUIRE(engine.questControl.started.empty());
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().allocations == 0);
        }
    }

    SECTION("when the call to start the quest does not go through")
    {
        engine.questControl.startCallSucceeds = false;
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should hand the slot back and put the sender down again")
        {
            // The promotion happened before the start attempt, so a sender
            // left at the designated rank would fill the NEXT letter's alias.
            REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
            REQUIRE(engine.factions.addToFactionCalls.back().rank == 0);
            REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE_FALSE(LetterPoolSpies().aborted.empty());
        }
    }

    SECTION("when the engine reports the quest did not start")
    {
        // The call goes through and the answer comes back false, which is a
        // different failure from the call itself not landing.
        engine.questControl.startResult = false;
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should hand the slot back")
        {
            REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
            REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE_FALSE(LetterPoolSpies().aborted.empty());
        }
    }
}

TEST_CASE("NPCLetterBeat confirms the letter arrived", "[NPCLetterBeat][engine]")
{
    // The RUNNING arm. Starting the quest is not evidence the letter exists:
    // the Papyrus side spawns it, gives it to the sender, and hands it to the
    // courier, and any of those can silently not happen.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;
    auto* letter = engine.AddReference(nullptr, kLetterRef, {});
    engine.FillRefAlias(quests.front().aliases.at(0), letter);
    engine.FillRefAlias(quests.front().aliases.at(1), letter);
    beat.OnStart(BeatContext{}, SenderParams());
    REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::RUNNING);

    SECTION("when the letter is in the courier's container")
    {
        SECTION("should finish the beat")
        {
            REQUIRE(RunArm(beat, BeatState::RUNNING) == BeatState::CLEANUP);
        }
    }

    SECTION("when the letter never turns up")
    {
        // Negative rather than zero: the book is not in the container at all,
        // which is what a dispatch that quietly did nothing looks like.
        engine.courier.inventoryCount = -1;

        SECTION("should give up once the verify window is out")
        {
            REQUIRE(RunArm(beat, BeatState::RUNNING) == BeatState::CLEANUP);
            REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().aborted == std::vector<std::size_t>{0});
        }
    }

    SECTION("when the world is not running normally")
    {
        engine.courier.inventoryCount = -1;

        SECTION("should not spend the verify window while it is")
        {
            // Eight seconds of verify window at a one-second poll is eight
            // ticks. Forty paused ticks must not consume any of them, or a
            // beat started before a long menu gives up during it.
            for (int i = 0; i < 40; ++i) {
                REQUIRE_FALSE(Tick(beat, BeatState::RUNNING, TickMode::Paused).transitionTo.has_value());
            }
            REQUIRE(engine.courier.getInventoryCountsCalls == 0);
        }
    }
}

TEST_CASE("NPCLetterBeat puts everything back when it is done", "[NPCLetterBeat][engine]")
{
    // CLEANUP runs on every path out of the beat, and what it does differs
    // entirely by whether the letter got out. Getting that backwards either
    // strands a slot or bars the beat for a day over a letter nobody received.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;
    engine.calendar.hoursPassed = 100.0f;
    auto* letter = engine.AddReference(nullptr, kLetterRef, {});
    engine.FillRefAlias(quests.front().aliases.at(0), letter);
    engine.FillRefAlias(quests.front().aliases.at(1), letter);
    beat.OnStart(BeatContext{}, SenderParams());
    REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::RUNNING);

    SECTION("when the letter reached the courier")
    {
        REQUIRE(RunArm(beat, BeatState::RUNNING) == BeatState::CLEANUP);
        REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);

        SECTION("should stamp the beat's cooldown")
        {
            REQUIRE(beat.RemainingCooldownGameHours() == 24.0);
        }

        SECTION("should hand the slot on to the pool's own lifecycle")
        {
            // Stage 20 is where the pool's own sinks take over: delivered,
            // read, disposed. A slot left below it never advances again.
            REQUIRE_FALSE(engine.papyrus.dispatches.empty());
            const auto& sent = engine.papyrus.dispatches.back();
            REQUIRE(sent.className == "Quest");
            REQUIRE(sent.methodName == "SetStage");
            REQUIRE(engine.papyrus.packedInts.back() == 20);
        }

        SECTION("should keep the slot rather than abort it")
        {
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().aborted.empty());
        }
    }

    SECTION("when it did not")
    {
        engine.courier.inventoryCount = -1;
        REQUIRE(RunArm(beat, BeatState::RUNNING) == BeatState::CLEANUP);
        REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);

        SECTION("should leave the cooldown unstamped")
        {
            // A day's silence bought by a letter the player never got is the
            // worst of both: no beat now, and no beat tomorrow either.
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }

        SECTION("should give the slot back")
        {
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().aborted == std::vector<std::size_t>{0});
        }
    }

    SECTION("when the beat is aborted mid-flight")
    {
        AbortFromMainThread(beat);

        SECTION("should roll the slot back rather than stamp a cooldown")
        {
            // Abort is the Director taking the beat away mid-run -- a shutdown
            // or a revert. It is always the failure branch, whatever the beat
            // had achieved.
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
            std::scoped_lock lock(LetterPoolSpies().mutex);
            REQUIRE(LetterPoolSpies().aborted == std::vector<std::size_t>{0});
        }
    }
}

TEST_CASE("NPCLetterBeat drives a slot's delivery quest", "[NPCLetterBeat][engine]")
{
    // The entry points the letter pool calls back into as the player carries
    // a letter around: stage advances for each step of its life, and the
    // teardown that takes one out of the world again.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LetterPoolSpies().Reset();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    auto* letter = engine.AddReference(nullptr, kLetterRef, {});
    constexpr std::size_t kSlot = 3;

    SECTION("when the pool reports a step in the letter's life")
    {
        QuestControl::AdvanceSlotStage(kSlot, 40);

        SECTION("should move that slot's quest to the matching stage")
        {
            // Read, in this case. The stage is what the quest's own Papyrus
            // fragments hang off, so a wrong number runs the wrong fragment.
            REQUIRE(engine.papyrus.dispatches.size() == 1);
            REQUIRE(engine.papyrus.dispatches.back().methodName == "SetStage");
            REQUIRE(engine.papyrus.packedInts.back() == 40);
            REQUIRE(engine.papyrus.handleRequests.back().form == static_cast<const void*>(quests.at(kSlot).quest));
        }
    }

    SECTION("when the allocator recycles a slot")
    {
        QuestControl::ShutdownSlotQuestSync(kSlot);

        SECTION("should stop and reset its quest on the spot")
        {
            // Natively rather than through the VM: the next dispatch on this
            // slot lands in the same frame, and the quest's own async shutdown
            // chain would still be running when it did.
            REQUIRE(engine.questControl.stopped == std::vector<const void*>{quests.at(kSlot).quest});
            REQUIRE(engine.questControl.reset == std::vector<const void*>{quests.at(kSlot).quest});
        }
    }

    SECTION("when a letter has to leave the world")
    {
        engine.FillRefAlias(quests.at(kSlot).aliases.at(1), letter);

        SECTION("should disable the reference the alias is holding")
        {
            // The alias tracks the letter wherever it has got to -- sender,
            // courier, player, a merchant's chest -- so this is the one handle
            // that reaches it.
            QuestControl::DeleteLetterRef(kSlot);
            REQUIRE(engine.questControl.disabled == std::vector<std::uint32_t>{kLetterRef});
            REQUIRE(letter->IsDeleted());
        }

        SECTION("should ask the courier to let go of it")
        {
            // Vanilla WICourier counts what it is carrying in a global. A
            // letter deleted from under it leaves that count permanently high
            // and the courier eventually stops coming.
            QuestControl::ReleaseLetterFromCourier(kSlot);
            REQUIRE_FALSE(engine.papyrus.dispatches.empty());
            const auto& sent = engine.papyrus.dispatches.back();
            REQUIRE(sent.className == "WICourierScript");
            REQUIRE(sent.methodName == "removeRefFromContainer");
            REQUIRE(engine.papyrus.packedForms.back() == static_cast<const void*>(letter));
            REQUIRE(engine.papyrus.packedBools.back() == false);
        }
    }

    SECTION("when the alias is holding nothing")
    {
        SECTION("should leave the world alone")
        {
            // The order matters in production: resetting the quest clears the
            // fill, after which this can only be a no-op. Reaching the engine
            // anyway would be disabling whatever the null resolved to.
            QuestControl::DeleteLetterRef(kSlot);
            QuestControl::ReleaseLetterFromCourier(kSlot);
            REQUIRE(engine.questControl.disabled.empty());
            REQUIRE(engine.papyrus.dispatches.empty());
        }
    }

    SECTION("when the slot is not one of the twenty")
    {
        SECTION("should do nothing at all")
        {
            QuestControl::AdvanceSlotStage(LetterPool::kPoolSize, 40);
            QuestControl::ShutdownSlotQuestSync(LetterPool::kPoolSize);
            QuestControl::DeleteLetterRef(LetterPool::kPoolSize);
            QuestControl::ReleaseLetterFromCourier(LetterPool::kPoolSize);
            REQUIRE(engine.papyrus.dispatches.empty());
            REQUIRE(engine.questControl.stopped.empty());
            REQUIRE(engine.questControl.disabled.empty());
        }
    }
}

TEST_CASE("NPCLetterBeat remembers who it has written to", "[NPCLetterBeat][engine]")
{
    // Two ledgers with different jobs. The cooldown decays and keeps one
    // person from writing twice in a week; the watermark never decays and
    // keeps them from writing twice about the same thing.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    engine.calendar.hoursPassed = 100.0f;

    SECTION("when a letter is delivered")
    {
        REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
        Cooldowns::OnLetterDelivered(kSender);

        SECTION("should take the sender off the candidate list")
        {
            REQUIRE(Cooldowns::IsSenderOnCooldown(kSender));
        }

        SECTION("should mark how far into their memories it read")
        {
            // Stamped after the delivery memory is written, so that memory
            // itself falls below the line and cannot become the subject of
            // the next letter from the same person.
            REQUIRE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender) == 100.0);
        }

        SECTION("should say nothing about anybody else")
        {
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender + 1));
            REQUIRE_FALSE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender + 1).has_value());
        }
    }

    SECTION("when the cooldown has run out")
    {
        Cooldowns::OnLetterDelivered(kSender);
        engine.calendar.hoursPassed = 200.0f;

        SECTION("should offer the sender again")
        {
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
        }

        SECTION("should keep the memory watermark anyway")
        {
            // The two are independent on purpose. A sender who is free to
            // write again still must not write about what they already wrote.
            REQUIRE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender) == 100.0);
        }
    }

    SECTION("when nobody is named")
    {
        SECTION("should record nothing")
        {
            // A delivery whose sender failed to resolve. Stamping zero would
            // put a real form on cooldown the first time one hashed there.
            Cooldowns::OnLetterDelivered(0);
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(0));
            REQUIRE_FALSE(Cooldowns::GetSenderMemoryWatermarkGameHours(0).has_value());
        }
    }
}

TEST_CASE("NPCLetterBeat carries its ledgers across a save", "[NPCLetterBeat][engine]")
{
    // Both ledgers and the beat's own clock go in one record. Losing any of
    // them lets the beat fire again the moment a save is loaded, which is how
    // a player gets three letters in a row after reloading.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto quests = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;
    engine.calendar.hoursPassed = 100.0f;

    SECTION("when the ledgers are written and read back")
    {
        // Both ledgers are keyed by FormID, and the ids in a co-save are the
        // ids of the load order that wrote it -- so both come back through
        // the migration table rather than as they were written. Keeping the
        // old ids would put whoever now sits at that index on cooldown.
        Cooldowns::OnLetterDelivered(kSender);
        SaveAndReload(engine);
        const auto migrated = engine.cosave.resolvedFormID;

        SECTION("should bring the per-sender cooldown back")
        {
            REQUIRE(Cooldowns::IsSenderOnCooldown(migrated));
        }

        SECTION("should bring the memory watermark back")
        {
            REQUIRE(Cooldowns::GetSenderMemoryWatermarkGameHours(migrated) == 100.0);
        }

        SECTION("should put the ids it read through the incoming load order")
        {
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
        }
    }

    SECTION("when a letter had just gone out")
    {
        SendOneLetter(beat, engine, quests);
        SaveAndReload(engine);

        SECTION("should bring the beat's own clock back")
        {
            REQUIRE(beat.RemainingCooldownGameHours() == 24.0);
        }
    }

    SECTION("when the save predates the watermark table")
    {
        // A v1 record carries the clock and the cooldowns and stops. The
        // reader must not go looking for bytes that were never written.
        Cooldowns::OnLetterDelivered(kSender);
        SaveAndReload(engine, 1);

        SECTION("should restore the cooldowns")
        {
            REQUIRE(Cooldowns::IsSenderOnCooldown(engine.cosave.resolvedFormID));
        }

        SECTION("should start the watermarks empty")
        {
            // Re-armed by the first delivery to each sender on the loaded
            // save, which is the cost of the upgrade and is bounded.
            REQUIRE_FALSE(Cooldowns::GetSenderMemoryWatermarkGameHours(engine.cosave.resolvedFormID).has_value());
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        Cooldowns::OnLetterDelivered(kSender);
        SaveAndReload(engine, 99);

        SECTION("should restore nothing")
        {
            // Reverting rather than keeping what was live: the live table
            // belongs to the save being left, not the one being entered.
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(engine.cosave.resolvedFormID));
        }
    }

    SECTION("when the record ends before the clock does")
    {
        Cooldowns::OnLetterDelivered(kSender);
        Persistence::OnSave(FakeInterface());
        engine.cosave.readable.clear();
        engine.cosave.readCursor = 0;
        Persistence::OnRevert();
        Persistence::OnLoad(FakeInterface(), kRecordVersion, 0);

        SECTION("should restore nothing")
        {
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(engine.cosave.resolvedFormID));
        }
    }

    SECTION("when there is no serialization interface")
    {
        Cooldowns::OnLetterDelivered(kSender);

        SECTION("should write nothing")
        {
            Persistence::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
        }

        SECTION("should leave what is live alone")
        {
            // Not the same as a failed load: nothing was read, so nothing
            // should have been cleared either.
            Persistence::OnLoad(nullptr, kRecordVersion, 0);
            REQUIRE(Cooldowns::IsSenderOnCooldown(kSender));
        }
    }

    SECTION("when the record will not open")
    {
        Cooldowns::OnLetterDelivered(kSender);
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            Persistence::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}

TEST_CASE("NPCLetterBeat without a courier to carry anything", "[NPCLetterBeat][engine]")
{
    // Its own case because the vanilla courier resolves once per process: a
    // world that never had one has to be the only world this process sees.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const NPCLetterBeat beat;

    SECTION("should not offer itself")
    {
        // Everything else is in place -- people to write, memories to write
        // about -- and it still cannot be offered, because nobody could carry
        // the letter. A load order without vanilla's WICourier, or one some
        // mod has broken it in.
        REQUIRE_FALSE(beat.IsAvailable(BeatContext{}));
    }
}

TEST_CASE("NPCLetterBeat with delivery quests that did not resolve", "[NPCLetterBeat][engine]")
{
    // Its own case for the same reason: the per-slot quest cache is filled
    // once, and a slot that resolved to nothing stays that way for the run.
    // This is what a botched install of the mod's own ESP looks like.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    engine.AddCourierQuest(true, true);
    engine.AddFaction(kSenderFaction, "_ne_LetterSenderFaction");
    engine.AddActor(kSender);
    engine.AddBook(kBook);
    engine.visibility.target3DPresent = false;
    engine.world.playerHasLocation = false;
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;

    SECTION("should still hand the pool a table, of nothing")
    {
        // The pool checks dispatchability against this table, so a beat that
        // never called it at all would leave the pool handing out slots that
        // can never be delivered.
        std::scoped_lock lock(LetterPoolSpies().mutex);
        REQUIRE(LetterPoolSpies().setPerSlotQuestsCalled);
        REQUIRE(LetterPoolSpies().perSlotQuests.front() == nullptr);
    }

    SECTION("should fail the dispatch rather than start a quest that is not there")
    {
        beat.OnStart(BeatContext{}, SenderParams());
        REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
        REQUIRE(engine.questControl.started.empty());
        std::scoped_lock lock(LetterPoolSpies().mutex);
        REQUIRE(LetterPoolSpies().aborted == std::vector<std::size_t>{0});
    }
}

TEST_CASE("NPCLetterBeat with delivery quests missing their aliases", "[NPCLetterBeat][engine]")
{
    // The quests exist and the aliases on them do not, which is an ESP a
    // patch has edited the alias records out of. Caught at dispatch rather
    // than at data load, because a slot with no aliases is still a slot the
    // pool may hand out.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    LetterPoolSpies().Reset();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    engine.AddCourierQuest(true, true);
    BuildPooledQuests(engine, LetterPool::kPoolSize, /*withAliases=*/false);
    engine.AddFaction(kSenderFaction, "_ne_LetterSenderFaction");
    engine.AddActor(kSender);
    engine.AddBook(kBook);
    engine.visibility.target3DPresent = false;
    engine.world.playerHasLocation = false;
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCLetterBeat beat;

    SECTION("should fail the dispatch rather than wait on a fill that cannot come")
    {
        // The alternative is five seconds of polling per slot for a fill that
        // is not coming, once per beat, forever.
        beat.OnStart(BeatContext{}, SenderParams());
        REQUIRE(RunArm(beat, BeatState::COMPOSE) == BeatState::CLEANUP);
        REQUIRE(engine.questControl.started.empty());
        std::scoped_lock lock(LetterPoolSpies().mutex);
        REQUIRE(LetterPoolSpies().aborted == std::vector<std::size_t>{0});
    }
}
