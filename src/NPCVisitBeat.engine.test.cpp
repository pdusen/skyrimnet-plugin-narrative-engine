#include <NPCVisitBeat.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>

#include <AsyncDispatch.h>
#include <FineRoads.h>
#include <IBeat.h>
#include <MainThread.h>
#include <PluginThread.h>
#include <SkyrimNetAPI.h>
#include <ThreadRole.h>
#include <VisitState.h>

#include <nlohmann/json.hpp>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Tests for the beat that sends somebody to the player in person.
//
// The beat is a state machine wrapped around a Papyrus quest it does not own.
// It asks the model for a briefing, starts the quest, and from then on reads
// the quest's stage every second and decides what to do about it: wait for the
// sender to walk over, hold the conversation while the world interrupts it,
// close it, and send them home again. Every one of those decisions is a gate
// on a distance, a timer, or a piece of live world state, and every one of
// them has a failure path that has to put the sender back where they started.
//
// Mocked-engine, and unusually deeply so: the quest and its three reference
// aliases, the sender's faction rank, positions, combat, the dialogue menu,
// line of sight, and the teleport home are all EngineMock's.
//
// The stage is read from the quest rather than tracked, so a test can put the
// beat at any stage by setting one and ticking -- which is how the stage
// handlers are reached without walking the whole quest each time.
//
// Two limits worth knowing. The harness answers IsInCombat globally, so a test
// cannot say whether it was the player or the sender who was fighting; the
// beat treats the two identically and the cases are named for that. And the
// stage timers are measured in wall-clock seconds the beat accumulates itself,
// so a case about a timeout costs the timeout.

namespace
{
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace Cooldowns = NarrativeEngine::NPCVisitBeat_Cooldowns;
    namespace Init = NarrativeEngine::NPCVisitBeat_Init;
    namespace Persistence = NarrativeEngine::NPCVisitBeat_Persistence;
    namespace Query = NarrativeEngine::NPCVisitBeat_Query;
    namespace VisitState = NarrativeEngine::VisitState;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::BeatPolarity;
    using NarrativeEngine::BeatState;
    using NarrativeEngine::NPCVisitBeat;
    using NarrativeEngine::TickMode;
    using NarrativeEngine::TickResult;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;

    // Every timer the stage handlers gate on is turned down to one second,
    // which is the floor the settings clamp to. A case about a timeout costs
    // that second of wall clock and there is no way to buy it back.
    constexpr const char* kSettings = "[General]\nbDebugMode=0\n"
                                      "[BeatSystem]\niBeatSystemPollIntervalMs=1000\n"
                                      "[Director]\niVisitMinSenderCandidates=1\n"
                                      "[Beats]\niVisitSenderCooldownGameHours=72\n"
                                      "iVisitApproachTimeoutSeconds=1\n"
                                      "iVisitSalutationApproachDistanceUnits=1000\n"
                                      "iVisitReEngageApproachDistanceUnits=1000\n"
                                      "iVisitValedictionDwellSeconds=1\n"
                                      "iVisitReturnHomeExitDistanceUnits=8000\n"
                                      "iVisitReturnHomeTimeoutSeconds=1\n"
                                      "iVisitOnHoldCombatMaxSeconds=1\n"
                                      "iVisitPollTurnCountThreshold=1\n"
                                      "iVisitPollSilenceRealSeconds=0\n"
                                      "iVisitPollMaxIntervalGameMinutes=0\n"
                                      "iVisitMaxIgnoreNudges=3\n"
                                      "iVisitBriefingMinWords=5\niVisitBriefingMaxWords=200\n"
                                      "iVisitMarkerMinDistanceUnits=800\n"
                                      "iVisitMarkerMaxDistanceUnits=2500\n"
                                      "iVisitArrivalCoverRadiusUnits=64\n"
                                      "bVisitArrivalAllowCoarseBearing=true\n"
                                      "iStuckRecoveryCheckIntervalSeconds=1\n"
                                      "iStuckRecoveryMovementThresholdUnits=100\n"
                                      "[FineRoads]\nbFineRoadsEnabled=1\n"
                                      "iFineRoadsBackstopSeconds=1\nbFineRoadsDebugBitmap=0\n";

    constexpr std::uint32_t kSender = 0x0001A6A0u;
    constexpr std::uint32_t kSenderBase = 0x0001A6A2u;
    constexpr std::uint32_t kVisitFaction = 0x0E0100FFu;
    constexpr std::uint32_t kVisitQuest = 0x0E010000u;
    constexpr std::uint32_t kReturnAnchor = 0x0E010A02u;
    constexpr std::uint32_t kXMarkerHeading = 0x00000034u;
    constexpr std::uint32_t kVisitWorldSpace = 0x0E010B01u;
    constexpr std::uint32_t kRoadMesh = 0x0E010B02u;
    constexpr std::uint32_t kGroundMesh = 0x0E010B03u;

    constexpr std::uint32_t kStageSalutation = 10;
    constexpr std::uint32_t kStageDiscuss = 20;
    constexpr std::uint32_t kStageValediction = 30;
    constexpr std::uint32_t kStageReturnHome = 50;
    constexpr std::uint32_t kStageRollback = 60;
    constexpr std::uint32_t kStageShutdown = 200;

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

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    // The narration is the scene the whole conversation is written from, so it
    // has a word floor of its own and a wider one than the briefing.
    std::string ComposeAnswer()
    {
        nlohmann::json j;
        j["briefing"] = "I have been turning this over for days and I mean to say it to your face.";
        j["narration"] = "She crosses the market with the walk of somebody who has decided something, stops "
                         "a pace short of you, and waits with her hands still until you look up from what "
                         "you were doing and give her your attention.";
        j["mood"] = "warm";
        j["topic_tag"] = "the tusk";
        return j.dump();
    }

    // The Director names the sender in the parameters, chosen from the
    // candidate list the action-select prompt renders for this beat.
    nlohmann::json SenderParams(const char* id = "0x1A6A0")
    {
        nlohmann::json params;
        params["sender_npc_form_id"] = id;
        return params;
    }

    std::string PollAnswer(bool shouldConclude)
    {
        nlohmann::json j;
        j["should_conclude"] = shouldConclude;
        j["rationale"] = "they have said what they came to say";
        j["closing_already_spoken"] = false;
        return j.dump();
    }

    void PrimeModel()
    {
        auto& fake = FakeLLM();
        fake.Reset();
        fake.memorySystemReady = true;
        SetJson(fake.engagementJson,
                R"([{"formId":)" + std::to_string(kSender)
                    + R"(,"name":"Ysolda","totalMemoryImportance":12.5,"lastEventTime":100.0}])");
        SetJson(fake.memoriesJson,
                R"([{"type":"EXPERIENCE","content":"She spoke of the mammoth tusk.","importance_score":0.8,)"
                R"("game_time":50.0,"emotion":"calm","location":"Whiterun"}])");
        SetJson(fake.promptResponse, ComposeAnswer());
    }

    // The visit world: the quest with its three aliases, the faction the
    // Sender alias fills from, a unique NPC to send, and the marker they are
    // put back at when the visit ends.
    struct VisitWorld
    {
        EngineMock::AliasedQuest quest;
        RE::Actor* sender = nullptr;
        RE::TESObjectREFR* returnAnchor = nullptr;
        RE::TESObjectCELL* cell = nullptr;
    };

    // A road running east through the player, with usable ground under it
    // and cover everywhere -- the smallest world in which VisitArrivalPoint
    // returns a Tier 1 answer. Which node it picks is VisitArrivalPoint's own
    // suite's business; here it only has to succeed, so the beat has
    // somewhere to warp the sender to.
    RE::TESObjectCELL* LayRoadWorld(EngineMock& engine)
    {
        auto* space = engine.AddWorldSpace(kVisitWorldSpace);
        auto* cell = engine.AddExteriorCell(space, 0, 0, nullptr);
        std::vector<EngineMock::FakeTriangle> road;
        for (int i = 0; i < 12; ++i) {
            EngineMock::FakeTriangle t;
            t.x = static_cast<float>(i) * 500.0f;
            t.y = 0.0f;
            t.z = 0.0f;
            t.neighbor[0] = i > 0 ? i - 1 : -1;
            t.neighbor[1] = i < 11 ? i + 1 : -1;
            road.push_back(t);
        }
        engine.AddNavMesh(cell, kRoadMesh, road);

        // Order matters twice over. FineRoads::Poll refuses to read the grid
        // while the player reads as indoors, so cellIsInterior has to be
        // false BEFORE the poll rather than after it. And StandPlayerInCell
        // copies that same flag onto the cell it fabricates, so changing it
        // afterwards does not reach back.
        engine.world.cellIsInterior = false;
        // The default mock position is off in the hundreds, which would
        // leave the road running past the player rather than away from them.
        engine.world.playerX = 0.0f;
        engine.world.playerY = 0.0f;
        engine.world.playerZ = 0.0f;
        engine.StandPlayerInCell(space, 0, 0);

        engine.LoadGrid({cell});
        {
            const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
            PluginThread::detail::JobDispatcher::Invoke(
                [](const PluginThread::Token& pt) { NarrativeEngine::FineRoads::Poll(pt, 1000.0); });
        }
        engine.terrain.landHeight = 0.0f;
        engine.AddNavmeshPatch(engine.GroundCell(), kGroundMesh, -20000.0f, -20000.0f, 20000.0f, 20000.0f, 0.0f);
        // Every ray blocked, so the cover gate never rejects and the search
        // has a Tier 1 answer to give.
        engine.visibility.pickHitFraction = 0.0f;
        return cell;
    }

    VisitWorld BuildWorld(EngineMock& engine, bool withAliases = true)
    {
        VisitWorld world;
        EngineMock::QuestState state;
        state.editorID = "_ne_VisitQuest";
        state.formID = kVisitQuest;
        state.running = false;
        const std::vector<std::string> aliases =
            withAliases ? std::vector<std::string>{"Sender", "ReturnAnchor"} : std::vector<std::string>{};
        world.quest = engine.AddQuestWithAliases(state, aliases);
        engine.AddFaction(kVisitFaction, "_ne_VisitSenderFaction");
        // Both runtime markers are placed from this one base, so without it
        // the beat cannot start at all.
        engine.AddStatic(kXMarkerHeading, "XMarkerHeading");
        world.cell = LayRoadWorld(engine);

        // The name the closing narration is written around, read off the
        // reference rather than the base record.
        engine.placement.actorName = "Ysolda";
        world.sender = engine.AddActor(kSender);
        auto* base = engine.AddNPC(kSenderBase, "Ysolda");
        base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
        engine.SetActorBase(world.sender, base);
        world.returnAnchor = engine.AddReference(nullptr, kReturnAnchor, {});
        // The sender needs somewhere to be, or the arrival search cannot
        // place their end of the route. The player's end is settled by
        // StandPlayerInCell inside LayRoadWorld.
        world.sender->parentCell = world.cell;
        return world;
    }

    // Stand the sender this far from the player, along one axis -- eastward,
    // which is the direction the test world's road runs.
    void PlaceSenderAway(EngineMock& engine, RE::Actor* sender, float units)
    {
        sender->SetPosition(RE::NiPoint3{engine.world.playerX + units, engine.world.playerY, engine.world.playerZ},
                            false);
    }

    // Stand in for the force-fills landing.
    //
    // The beat dispatches FillSenderSlot and FillReturnAnchorSlot into the
    // Papyrus VM, which the harness records but does not run, so nothing
    // would ever fill these. Pre-filling them means VerifyingFill sees what
    // it would have seen a tick after a real dispatch. That the dispatch was
    // made at all is asserted separately -- this helper only lets the
    // machine get past the readback.
    void FillAliases(EngineMock& engine, const VisitWorld& world)
    {
        engine.FillRefAlias(world.quest.aliases.at(0), world.sender);
        engine.FillRefAlias(world.quest.aliases.at(1), world.returnAnchor);
    }

    TickResult Tick(NPCVisitBeat& beat, BeatState state, TickMode mode = TickMode::Normal)
    {
        TickResult result;
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { result = beat.Tick(pt, mode, state); });
        return result;
    }

    // One evaluation of the stage machine. The RUNNING arm marshals a stage
    // tick every fourth worker tick, so a case that wants the machine to look
    // at the world once has to tick it four times.
    TickResult StageTick(NPCVisitBeat& beat, TickMode mode = TickMode::Normal)
    {
        TickResult last;
        for (int i = 0; i < 4; ++i) {
            if (const auto result = Tick(beat, BeatState::RUNNING, mode); result.transitionTo)
                last = result;
        }
        return last;
    }

    // Tick for a stretch of wall clock, which is the only timebase the stage
    // timers have. Returns the last transition asked for, if any.
    TickResult TickForSeconds(NPCVisitBeat& beat, BeatState state, TickMode mode, double seconds)
    {
        TickResult last;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto result = Tick(beat, state, mode); result.transitionTo)
                last = result;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return last;
    }

    // Drive COMPOSE until it asks to move on. The briefing comes back on the
    // dispatch thread, so ticking is the only way to learn that it has.
    BeatState RunCompose(NPCVisitBeat& beat, int maxTicks = 400)
    {
        for (int i = 0; i < maxTicks; ++i) {
            if (const auto result = Tick(beat, BeatState::COMPOSE); result.transitionTo)
                return *result.transitionTo;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return BeatState::COMPOSE;
    }

    // Compose the visit, then stand the sender `units` from the player.
    //
    // The two cannot be the same distance. Composing needs a journey to
    // stage -- a sender standing on top of the player has no road between
    // them and the arrival search rightly declines -- while the stage
    // machine under test needs them already arrived. In the game the beat
    // closes that gap by warping them in and letting them walk; here it is
    // closed directly.
    void ComposeWithSenderStandingAt(NPCVisitBeat& beat, EngineMock& engine, const VisitWorld& world, float units)
    {
        PlaceSenderAway(engine, world.sender, 3000.0f);
        beat.OnStart(BeatContext{}, SenderParams());
        REQUIRE(RunCompose(beat) == BeatState::RUNNING);
        PlaceSenderAway(engine, world.sender, units);
    }

    // Every reference the beat placed that is still in the world. The arrival
    // marker is meant to be gone by the time COMPOSE is done; the return
    // anchor is meant to outlive it.
    std::size_t SurvivingPlacedRefs(const EngineMock& engine)
    {
        std::size_t alive = 0;
        for (const auto& placement : engine.spawn.placed) {
            auto* form = RE::TESForm::LookupByID(placement.refFormID);
            if (form && !form->IsDeleted()) {
                ++alive;
            }
        }
        return alive;
    }

    // Tick the RUNNING arm long enough for the escort to check twice.
    //
    // Its clock is wall-clock elapsed and Settings clamps the interval to a
    // one-second floor, so this cannot be hurried: the first check only
    // establishes where the sender was, and the second is the one that can
    // tell they have not moved since.
    void EscortTicks(NPCVisitBeat& beat)
    {
        TickForSeconds(beat, BeatState::RUNNING, TickMode::Normal, 2.4);
    }

    void AbortFromMainThread(NPCVisitBeat& beat)
    {
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            NarrativeEngine::MainThread::Run(pt, [&](const NarrativeEngine::MainThread::Token& mt) { beat.Abort(mt); });
        });
    }

    bool DispatchedMethod(const EngineMock& engine, const std::string& method)
    {
        for (const auto& sent : engine.papyrus.dispatches) {
            if (sent.methodName == method)
                return true;
        }
        return false;
    }

    bool DispatchedStage(const EngineMock& engine, std::int32_t stage)
    {
        for (std::size_t i = 0; i < engine.papyrus.dispatches.size(); ++i) {
            if (engine.papyrus.dispatches[i].methodName != "SetStage")
                continue;
            for (const auto packed : engine.papyrus.packedInts) {
                if (packed == stage)
                    return true;
            }
        }
        return false;
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

TEST_CASE("NPCVisitBeat says what it is", "[NPCVisitBeat][engine]")
{
    const NPCVisitBeat beat;

    SECTION("should answer to the name the Director picks it by")
    {
        REQUIRE(beat.Name() == "npc_visit");
    }

    SECTION("should serve either direction")
    {
        // A visit can be a warm apology or a threat delivered in person, so
        // the Director may reach for it whichever way it wants tension to go.
        REQUIRE(beat.Polarity() == BeatPolarity::Either);
    }

    SECTION("should say which of the two social beats to reach for")
    {
        // The description IS the prompt the picker reads, and the letter beat
        // covers the same ground. Without the comparison the two read as
        // interchangeable and the Director picks by coin toss.
        REQUIRE(beat.Description().find("npc_letter") != std::string::npos);
        REQUIRE(beat.Description().find("urgency_hint") != std::string::npos);
    }
}

TEST_CASE("NPCVisitBeat says when it can be offered", "[NPCVisitBeat][engine]")
{
    // Happy path, re-run per leaf: the quest and its aliases resolved, a
    // memory system with somebody in it who could plausibly call, and a player
    // standing somewhere a visitor would not look absurd.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const NPCVisitBeat beat;
    BeatContext ctx;

    SECTION("when nothing is in the way")
    {
        SECTION("should offer itself")
        {
            REQUIRE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the player is somewhere a stranger walking up would be absurd")
    {
        auto* barrow = engine.AddLocation(0x0E010B01u, "Bleak Falls Barrow", {"LocTypeBanditCamp"});
        engine.world.playerLocationOverride = barrow;
        ctx.player = engine.AddActor(0x00000014u);

        SECTION("should stay out of it")
        {
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the memory system is not up")
    {
        FakeLLM().memorySystemReady = false;

        SECTION("should stay out of it")
        {
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when nobody engaged could actually come")
    {
        // Present in the engagement list and in combat, which is one of the
        // four things the viability walk rejects. The count has to agree with
        // what the composer would end up building, or the Director offers a
        // beat that then fails to start.
        engine.player.inCombat = true;

        SECTION("should stay out of it")
        {
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }
}

TEST_CASE("NPCVisitBeat sends somebody to the player", "[NPCVisitBeat][engine]")
{
    // The COMPOSE arm: ask the model for a briefing, remember where the sender
    // was standing, promote them into the alias faction, and start the quest.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);
    world.sender->SetPosition(RE::NiPoint3{4000.0f, 5000.0f, 600.0f}, false);
    beat.OnStart(BeatContext{}, SenderParams());

    SECTION("when the quest starts with its aliases filled")
    {
        const auto next = RunCompose(beat);

        SECTION("should go on to run the visit")
        {
            REQUIRE(next == BeatState::RUNNING);
        }

        SECTION("should keep what the model wrote for the scene to be played from")
        {
            // The narration is spoken when the sender arrives and the briefing
            // is what every dialogue turn after that is generated against.
            // Neither is asked for a second time.
            const auto snap = VisitState::GetSnapshot();
            REQUIRE(snap.topicTag == "the tusk");
            REQUIRE(snap.mood == "warm");
            REQUIRE(snap.narrationText.starts_with("She crosses the market"));
            REQUIRE_FALSE(snap.briefingText.empty());
        }

        SECTION("should remember where the sender was standing")
        {
            // The whole point of the visit is that it puts the world back. A
            // sender warped in from across the hold and never given their
            // position back is a permanently displaced NPC.
            const auto snap = VisitState::GetSnapshot();
            REQUIRE(snap.returnPosition.x == 4000.0f);
            REQUIRE(snap.returnPosition.y == 5000.0f);
            // The anchor is a marker the beat places at the sender's own
            // position, so what matters is that one exists -- its FormID is
            // assigned at runtime and is not knowable here.
            REQUIRE(snap.returnAnchorFormID != 0);
        }

        SECTION("should put the sender at the rank the alias fills from")
        {
            REQUIRE_FALSE(engine.factions.addToFactionCalls.empty());
            const auto& call = engine.factions.addToFactionCalls.back();
            REQUIRE(call.actorFormID == kSender);
            REQUIRE(call.factionFormID == kVisitFaction);
            REQUIRE(call.rank == 4);
        }

        SECTION("should start the visit quest")
        {
            REQUIRE(engine.questControl.started == std::vector<const void*>{world.quest.quest});
        }
    }

    SECTION("when the Director named a sender that will not parse")
    {
        beat.OnStart(BeatContext{}, SenderParams("not a form id"));

        SECTION("should fail on the first tick without asking the model")
        {
            REQUIRE(Tick(beat, BeatState::COMPOSE).transitionTo == BeatState::CLEANUP);
            REQUIRE(FakeLLM().sendPromptCalls == 0);
        }
    }

    SECTION("when the model refuses to write a briefing")
    {
        FakeLLM().sendPromptSucceeds = false;

        SECTION("should give up without starting anything")
        {
            REQUIRE(RunCompose(beat) == BeatState::CLEANUP);
            REQUIRE(engine.questControl.started.empty());
        }
    }

    SECTION("when the sender has died while the briefing was being written")
    {
        engine.forms.actorIsDead = true;

        SECTION("should give up without promoting anybody")
        {
            REQUIRE(RunCompose(beat) == BeatState::CLEANUP);
            REQUIRE(engine.factions.addToFactionCalls.empty());
        }
    }

    SECTION("when the quest refuses to start")
    {
        engine.questControl.startResult = false;

        SECTION("should put the sender back down to candidate rank")
        {
            // The promotion has already happened by then, and a sender left
            // designated would fill the alias on somebody else's visit.
            REQUIRE(RunCompose(beat) == BeatState::CLEANUP);
            REQUIRE(engine.factions.addToFactionCalls.back().rank == 0);
        }
    }

    SECTION("when the quest starts but leaves an alias empty")
    {
        // The engine reports success and the Papyrus side has not actually
        // filled anybody in. Nothing later would notice: the stage handlers
        // read the alias and return quietly when it is null, so the beat
        // would sit at Salutation until its timeout.
        engine.FillRefAlias(world.quest.aliases.at(1), nullptr);

        SECTION("should roll the quest back rather than run it")
        {
            REQUIRE(RunCompose(beat) == BeatState::CLEANUP);
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageRollback)));
        }
    }
}

TEST_CASE("NPCVisitBeat gives up cleanly when the arrival cannot be staged", "[NPCVisitBeat][engine]")
{
    // COMPOSE is now six steps, and each of them can fail. What matters is
    // not that they can -- it is that every one of them ends the same way:
    // CLEANUP, no quest left running, and nothing left standing in the world.
    // A beat that fails halfway and leaves a marker behind is how a save
    // accumulates junk nobody can trace.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;

    SECTION("when the player can see every piece of road they might come up")
    {
        // Open ground with nothing to stand behind. The search declines, and
        // declining is the whole point: arriving in plain sight would be the
        // tell this beat exists to remove.
        engine.visibility.pickHitFraction = 1.0f;
        FillAliases(engine, world);
        PlaceSenderAway(engine, world.sender, 3000.0f);
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should decline before the quest is ever started")
        {
            REQUIRE(RunCompose(beat) == BeatState::CLEANUP);
            // Ahead of EnsureQuestStarted on purpose: failing here means
            // there is no quest to tear down afterwards.
            REQUIRE(engine.questControl.started.empty());
            REQUIRE(engine.spawn.placed.empty());
        }
    }

    SECTION("when the arrival marker cannot be placed")
    {
        // One placement succeeds -- the return anchor, made first -- and the
        // next one, the marker the sender is warped onto, does not.
        engine.spawn.placeSucceedsCount = 1;
        FillAliases(engine, world);
        PlaceSenderAway(engine, world.sender, 3000.0f);
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should give up rather than leave the sender where they were")
        {
            REQUIRE(RunCompose(beat) == BeatState::CLEANUP);
            REQUIRE_FALSE(engine.questControl.teleports.size() > 0);
        }
    }

    SECTION("when the sender's own alias never fills")
    {
        // The dispatch into Papyrus is fire-and-forget: it reports that the
        // call was queued, not that it ran. A fill that never lands has to
        // time out rather than leave the beat waiting on it forever.
        engine.FillRefAlias(world.quest.aliases.at(1), world.returnAnchor);
        PlaceSenderAway(engine, world.sender, 3000.0f);
        beat.OnStart(BeatContext{}, SenderParams());

        SECTION("should roll the quest back rather than run a visit with nobody in it")
        {
            REQUIRE(RunCompose(beat) == BeatState::CLEANUP);
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageRollback)));
        }
    }
}

TEST_CASE("NPCVisitBeat moves a visitor who cannot walk the last stretch", "[NPCVisitBeat][engine]")
{
    // The arrival point is vetted, but vetting a POSITION is not the same as
    // proving a route: the search asks whether someone can stand somewhere,
    // while the engine has to walk a collision capsule from there to the
    // player. Where those two disagree the sender simply never arrives, and
    // before this the beat just burned its approach timeout and rolled back.
    //
    // The fallbacks are what makes recovery possible, and where they come
    // from is the point -- each is another node further along the same road,
    // so escalating walks the visitor BACK ALONG THEIR OWN ROUTE rather than
    // sideways onto ground nothing has vetted.
    EngineMock engine;
    const std::string patientSettings = std::string{kSettings} + "[Beats]\niVisitApproachTimeoutSeconds=30\n";
    const ConfiguredSettings settings{patientSettings.c_str()};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);

    PlaceSenderAway(engine, world.sender, 3000.0f);
    beat.OnStart(BeatContext{}, SenderParams());
    REQUIRE(RunCompose(beat) == BeatState::RUNNING);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageSalutation);

    SECTION("when they have stopped moving well short of the player")
    {
        // Far enough out to still be approaching rather than arrived, and
        // then not moving at all between checks.
        PlaceSenderAway(engine, world.sender, 3000.0f);
        const auto stalledAt = world.sender->GetPosition();
        EscortTicks(beat);

        SECTION("should put them somewhere else on the road in")
        {
            REQUIRE(world.sender->GetPosition().x != stalledAt.x);
        }
    }

    SECTION("when they are already close enough to be arriving")
    {
        PlaceSenderAway(engine, world.sender, 100.0f);
        const auto standingAt = world.sender->GetPosition();
        EscortTicks(beat);

        SECTION("should leave them alone")
        {
            // Stillness this close is someone standing in front of the
            // player about to speak, not someone stuck. Warping them here
            // would be the visible bug the escort exists to prevent.
            REQUIRE(world.sender->GetPosition().x == standingAt.x);
        }
    }
}

TEST_CASE("NPCVisitBeat waits for the fills it asked for", "[NPCVisitBeat][engine]")
{
    // The readback is a whole sub-phase of its own rather than a line at the
    // end of the warp, because VMDispatchOnQuest only ever says the call was
    // QUEUED. Reading the aliases back in the same tick would fail every
    // time; reading them once and giving up would fail whenever the VM was
    // busy.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;

    PlaceSenderAway(engine, world.sender, 3000.0f);
    beat.OnStart(BeatContext{}, SenderParams());

    // Tick with the aliases still empty. The beat has asked Papyrus to fill
    // them and is waiting for an answer that has not come yet.
    for (int i = 0; i < 6; ++i) {
        REQUIRE_FALSE(Tick(beat, BeatState::COMPOSE).transitionTo.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    SECTION("when the fills land late")
    {
        FillAliases(engine, world);

        SECTION("should pick them up and run the visit")
        {
            REQUIRE(RunCompose(beat) == BeatState::RUNNING);
        }

        SECTION("should have asked Papyrus for both of them")
        {
            REQUIRE(RunCompose(beat) == BeatState::RUNNING);
            // ForceRefTo has no native binding, so these two dispatches are
            // the only way either alias is ever filled.
            REQUIRE(DispatchedMethod(engine, "FillSenderSlot"));
            REQUIRE(DispatchedMethod(engine, "FillReturnAnchorSlot"));
        }

        SECTION("should have thrown away the marker it warped them onto")
        {
            REQUIRE(RunCompose(beat) == BeatState::RUNNING);
            // Two references were placed: the anchor, which the visit needs
            // for the walk home, and the arrival marker, which existed only
            // to be a MoveTo target. Exactly one should still be standing.
            REQUIRE(engine.spawn.placed.size() == 2);
            REQUIRE(SurvivingPlacedRefs(engine) == 1);
        }
    }
}

TEST_CASE("NPCVisitBeat waits for the sender to walk over", "[NPCVisitBeat][engine]")
{
    // The Salutation stage. The sender has been warped to a marker out of
    // sight and is walking in under a Follow package; the beat watches the
    // distance close and has to decide when they have arrived, or that they
    // never will.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);
    engine.calendar.hoursPassed = 100.0f;
    PlaceSenderAway(engine, world.sender, 3000.0f);
    beat.OnStart(BeatContext{}, SenderParams());
    REQUIRE(RunCompose(beat) == BeatState::RUNNING);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageSalutation);
    engine.papyrus.dispatches.clear();
    engine.papyrus.packedInts.clear();
    engine.papyrus.packedStrings.clear();

    SECTION("when the sender is still on their way")
    {
        SECTION("should leave them to it")
        {
            StageTick(beat);
            REQUIRE(engine.papyrus.dispatches.empty());
        }
    }

    SECTION("when the sender arrives")
    {
        PlaceSenderAway(engine, world.sender, 100.0f);
        StageTick(beat);

        SECTION("should speak the line the model wrote for their arrival")
        {
            REQUIRE(DispatchedMethod(engine, "RunSenderNarration"));
            REQUIRE_FALSE(engine.papyrus.packedStrings.empty());
            REQUIRE(engine.papyrus.packedStrings.front().starts_with("She crosses the market"));
        }

        SECTION("should move the quest on to the conversation")
        {
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageDiscuss)));
        }

        SECTION("should stamp the sender's cooldown")
        {
            // Stamped on arrival rather than at the end: this is the moment
            // the visit became something the player experienced, and a visit
            // that then goes wrong should still not repeat tomorrow.
            REQUIRE(Cooldowns::IsSenderOnCooldown(kSender));
        }

        SECTION("should not speak the line twice while the stage change lands")
        {
            // The Papyrus stage change is asynchronous, so the stage stays at
            // Salutation for a few more ticks. Without the latch the beat
            // re-fires the arrival narration every second until it lands.
            StageTick(beat);
            StageTick(beat);
            int narrations = 0;
            for (const auto& sent : engine.papyrus.dispatches) {
                if (sent.methodName == "RunSenderNarration")
                    ++narrations;
            }
            REQUIRE(narrations == 1);
        }
    }

    SECTION("when the sender never arrives")
    {
        TickForSeconds(beat, BeatState::RUNNING, TickMode::Normal, 1.5);

        SECTION("should send them back where they came from")
        {
            REQUIRE_FALSE(engine.questControl.teleports.empty());
            REQUIRE(engine.questControl.teleports.back().moverFormID == kSender);
            REQUIRE(engine.questControl.teleports.back().destinationFormID == kReturnAnchor);
        }

        SECTION("should put them back down to candidate rank")
        {
            REQUIRE(engine.factions.addToFactionCalls.back().rank == 0);
        }

        SECTION("should roll the quest back")
        {
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageRollback)));
        }

        SECTION("should record the visit as rolled back rather than done")
        {
            // The dashboard's history is the only place a player-visible
            // account of what the beat did survives.
            const auto history = VisitState::GetHistory();
            REQUIRE_FALSE(history.empty());
            REQUIRE(history.front().outcome == VisitState::Outcome::RolledBack);
        }

        SECTION("should not stamp the sender's cooldown")
        {
            // Nothing happened as far as the player is concerned, so the
            // sender stays eligible.
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
        }
    }
}

TEST_CASE("NPCVisitBeat holds the conversation while the world interrupts", "[NPCVisitBeat][engine]")
{
    // The Discuss stage runs a three-way cycle inside one quest stage. A
    // conversation cannot survive a dragon attack or the player opening a
    // dialogue menu with somebody else, so it pauses and picks itself back up.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);
    ComposeWithSenderStandingAt(beat, engine, world, 100.0f);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageDiscuss);

    SECTION("when nothing is interrupting")
    {
        SECTION("should be talking")
        {
            StageTick(beat);
            REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::Discussing);
            REQUIRE(VisitState::DerivePhase() == VisitState::Mode::Discuss);
        }
    }

    SECTION("when a fight breaks out")
    {
        engine.player.inCombat = true;
        StageTick(beat);

        SECTION("should put the conversation on hold")
        {
            REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::OnHold);
            REQUIRE(VisitState::DerivePhase() == VisitState::Mode::OnHold);
        }

        SECTION("should try to pick it back up once the fight is over")
        {
            // Not straight back to talking: whoever was fighting has moved,
            // so the sender has to walk back before the conversation resumes.
            engine.player.inCombat = false;
            StageTick(beat);
            REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::ReEngage);
            REQUIRE(VisitState::DerivePhase() == VisitState::Mode::ReEngage);
        }

        SECTION("should give up on a fight that never ends")
        {
            // The watchdog. Without it a visit interrupted by an unwinnable
            // fight holds its quest, its alias and its faction rank forever.
            TickForSeconds(beat, BeatState::RUNNING, TickMode::Combat, 1.5);
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageShutdown)));
            const auto history = VisitState::GetHistory();
            REQUIRE_FALSE(history.empty());
            REQUIRE(history.front().outcome == VisitState::Outcome::Aborted);
        }
    }

    SECTION("when the player is talking to somebody else")
    {
        engine.ui.openMenus.emplace_back("Dialogue Menu");

        SECTION("should put the conversation on hold")
        {
            // The vanilla dialogue menu is modal. Generating turns behind it
            // means the sender talks to a player who cannot hear them.
            StageTick(beat);
            REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::OnHold);
        }
    }

    SECTION("when the sender has walked back after an interruption")
    {
        engine.player.inCombat = true;
        StageTick(beat);
        engine.player.inCombat = false;
        StageTick(beat);
        REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::ReEngage);
        engine.papyrus.dispatches.clear();
        engine.papyrus.packedStrings.clear();
        StageTick(beat);

        SECTION("should say something about resuming rather than arriving again")
        {
            // Replaying the composed opener would narrate the sender walking
            // up for a second time, in the middle of a conversation they are
            // already having.
            REQUIRE(DispatchedMethod(engine, "RunSenderNarration"));
            REQUIRE_FALSE(engine.papyrus.packedStrings.empty());
            REQUIRE(engine.papyrus.packedStrings.front().starts_with("Now that the interruption has ended"));
        }

        SECTION("should go back to talking")
        {
            REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::Discussing);
        }
    }

    SECTION("when the interruption returns while the sender is walking back")
    {
        engine.player.inCombat = true;
        StageTick(beat);
        engine.player.inCombat = false;
        StageTick(beat);
        engine.player.inCombat = true;

        SECTION("should put it back on hold")
        {
            StageTick(beat);
            REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::OnHold);
        }
    }
}

TEST_CASE("NPCVisitBeat closes the conversation", "[NPCVisitBeat][engine]")
{
    // The one path that goes the whole way: the sender arrives, a turn is
    // spoken, the poll is asked whether they are done, and the answer walks
    // the quest through Valediction to ReturnHome. Reached end to end rather
    // than by setting stages, because arming the poll and stamping the
    // watermark both happen on the way and neither has another door.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);
    ComposeWithSenderStandingAt(beat, engine, world, 100.0f);

    // Arrive, which is what arms the poll and starts the speech sampler.
    engine.courier.questStage = static_cast<std::uint16_t>(kStageSalutation);
    StageTick(beat);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageDiscuss);
    // One turn spoken since the sampler's cursor, which is the gate the poll
    // fires on. The clock starts at zero, so any positive time is new.
    SetJson(FakeLLM().dialogueJson, R"([{"gameTime":100.0,"speaker":"player","text":"go on"}])");
    engine.papyrus.dispatches.clear();
    engine.papyrus.packedInts.clear();
    engine.papyrus.packedStrings.clear();

    SECTION("when the poll says they are done")
    {
        SetJson(FakeLLM().promptResponse, PollAnswer(true));
        TickForSeconds(beat, BeatState::RUNNING, TickMode::Normal, 0.5);

        SECTION("should move the quest on to the closing")
        {
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageValediction)));
        }

        SECTION("should give the sender a line to leave on")
        {
            // Named rather than "the sender": the closing beat is the last
            // thing the player hears and it is spoken about somebody they
            // have been talking to for minutes.
            REQUIRE(DispatchedMethod(engine, "RunSenderNarration"));
            bool named = false;
            for (const auto& packed : engine.papyrus.packedStrings) {
                if (packed.find("Ysolda") != std::string::npos && packed.find("take their leave") != std::string::npos)
                    named = true;
            }
            REQUIRE(named);
        }

        SECTION("should mark how far into the sender's memories the visit read")
        {
            // Stamped here rather than on arrival: this is the moment the
            // visit landed, and the topic is only spent once it has.
            REQUIRE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender).has_value());
        }

        SECTION("should send them home once they have stood there long enough")
        {
            // The dwell is what stops the sender turning on their heel the
            // instant the closing line is dispatched.
            engine.courier.questStage = static_cast<std::uint16_t>(kStageValediction);
            engine.papyrus.packedInts.clear();
            TickForSeconds(beat, BeatState::RUNNING, TickMode::Normal, 1.5);
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageReturnHome)));
        }
    }

    SECTION("when the poll says they are not")
    {
        SetJson(FakeLLM().promptResponse, PollAnswer(false));
        TickForSeconds(beat, BeatState::RUNNING, TickMode::Normal, 0.5);

        SECTION("should leave the conversation running")
        {
            REQUIRE_FALSE(DispatchedStage(engine, static_cast<std::int32_t>(kStageValediction)));
            REQUIRE(Query::GetDiscussSubPhase() == Query::DiscussSubPhase::Discussing);
        }
    }

    SECTION("when the poll keeps failing to answer")
    {
        // Six broken answers in a row and the beat gives up rather than hold
        // the sender in a conversation nothing can end.
        const ConfiguredSettings impatient{"[Beats]\niVisitConclusionPollMaxConsecutiveFailures=1\n"
                                           "iVisitPollTurnCountThreshold=1\niVisitPollSilenceRealSeconds=0\n"
                                           "iVisitPollMaxIntervalGameMinutes=0\n"};
        SetJson(FakeLLM().promptResponse, "not json at all");

        SECTION("should abandon the visit")
        {
            TickForSeconds(beat, BeatState::RUNNING, TickMode::Normal, 0.8);
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageShutdown)));
        }
    }
}

TEST_CASE("NPCVisitBeat sends the visitor home", "[NPCVisitBeat][engine]")
{
    // The ReturnHome stage. The sender is walking back under their own
    // package and the beat is watching for any sign it is safe to stop
    // caring: distance, an unloaded cell, or the player losing sight of them.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);
    ComposeWithSenderStandingAt(beat, engine, world, 100.0f);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageReturnHome);

    SECTION("when the sender is still in sight and nearby")
    {
        SECTION("should let them keep walking")
        {
            // Teleporting them the moment the stage starts would have an NPC
            // vanish in front of the player.
            //
            // Counted from after COMPOSE rather than from zero: the arrival
            // warp is a teleport too, and it has already happened by now.
            const auto before = engine.questControl.teleports.size();
            StageTick(beat);
            REQUIRE(engine.questControl.teleports.size() == before);
        }
    }

    SECTION("when the sender has got far enough away")
    {
        PlaceSenderAway(engine, world.sender, 9000.0f);
        StageTick(beat);

        SECTION("should put them back where they were")
        {
            REQUIRE_FALSE(engine.questControl.teleports.empty());
            REQUIRE(engine.questControl.teleports.back().destinationFormID == kReturnAnchor);
        }

        SECTION("should put them back down to candidate rank")
        {
            REQUIRE(engine.factions.addToFactionCalls.back().rank == 0);
        }

        SECTION("should shut the quest down")
        {
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageShutdown)));
        }

        SECTION("should record the visit as finished")
        {
            const auto history = VisitState::GetHistory();
            REQUIRE_FALSE(history.empty());
            REQUIRE(history.front().outcome == VisitState::Outcome::Completed);
        }

        SECTION("should ask to be cleaned up")
        {
            REQUIRE(StageTick(beat).transitionTo == BeatState::CLEANUP);
        }
    }

    SECTION("when the cell the sender is in unloads")
    {
        // They walked through a door, or the player did. Nothing more is going
        // to happen to them where the player can see it.
        auto* cell = engine.GroundCell();
        world.sender->parentCell = cell;
        engine.SetCellAttached(cell, false);

        SECTION("should finish there")
        {
            StageTick(beat);
            REQUIRE_FALSE(engine.questControl.teleports.empty());
        }
    }

    SECTION("when the player can no longer see them")
    {
        // Distance alone is not enough for this one: an NPC that vanishes at
        // two thousand units in plain sight is worse than one who walks.
        PlaceSenderAway(engine, world.sender, 3000.0f);
        engine.visibility.target3DPresent = false;

        SECTION("should finish there")
        {
            StageTick(beat);
            REQUIRE_FALSE(engine.questControl.teleports.empty());
        }
    }

    SECTION("when the return anchor was never filled")
    {
        engine.FillRefAlias(world.quest.aliases.at(1), nullptr);
        PlaceSenderAway(engine, world.sender, 9000.0f);

        SECTION("should put them back where they were standing anyway")
        {
            // There is no reference left to move them onto, but the
            // snapshot still knows where the beat found them. This used to
            // be a MoveTo onto the sender's own reference, which moves
            // nobody anywhere and left them wherever the visit ended.
            const auto snap = VisitState::GetSnapshot();
            StageTick(beat);
            REQUIRE(world.sender->GetPosition().x == snap.returnPosition.x);
            REQUIRE(world.sender->GetPosition().y == snap.returnPosition.y);
        }
    }
}

TEST_CASE("NPCVisitBeat freezes when the world is not running", "[NPCVisitBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);
    PlaceSenderAway(engine, world.sender, 9000.0f);
    beat.OnStart(BeatContext{}, SenderParams());

    SECTION("when the game is paused")
    {
        SECTION("should not begin composing")
        {
            for (int i = 0; i < 40; ++i) {
                REQUIRE_FALSE(Tick(beat, BeatState::COMPOSE, TickMode::Paused).transitionTo.has_value());
            }
            REQUIRE(FakeLLM().sendPromptCalls == 0);
        }
    }

    SECTION("when the player is in a menu conversation")
    {
        SECTION("should not begin composing")
        {
            for (int i = 0; i < 40; ++i) {
                REQUIRE_FALSE(Tick(beat, BeatState::COMPOSE, TickMode::Dialogue).transitionTo.has_value());
            }
            REQUIRE(FakeLLM().sendPromptCalls == 0);
        }
    }

    SECTION("when a fight breaks out mid-compose")
    {
        SECTION("should wait rather than compose during it")
        {
            // Combat does not freeze the whole beat -- the Discuss watchdog
            // needs to run under it -- but the COMPOSE arm still holds.
            for (int i = 0; i < 40; ++i) {
                REQUIRE_FALSE(Tick(beat, BeatState::COMPOSE, TickMode::Combat).transitionTo.has_value());
            }
            REQUIRE(FakeLLM().sendPromptCalls == 0);
        }
    }

    SECTION("when the game is paused during the visit")
    {
        REQUIRE(RunCompose(beat) == BeatState::RUNNING);
        engine.courier.questStage = static_cast<std::uint16_t>(kStageReturnHome);

        SECTION("should not act on the world while it is")
        {
            // The sender is far enough away to be sent home the instant the
            // stage machine looks at them, so any tick that reaches it shows.
            const auto before = engine.questControl.teleports.size();
            for (int i = 0; i < 40; ++i) {
                Tick(beat, BeatState::RUNNING, TickMode::Paused);
            }
            REQUIRE(engine.questControl.teleports.size() == before);
        }
    }
}

TEST_CASE("NPCVisitBeat tears the visit down", "[NPCVisitBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    const auto world = BuildWorld(engine);
    Init::Initialize();
    const RunningDispatch dispatch;
    NPCVisitBeat beat;
    FillAliases(engine, world);
    ComposeWithSenderStandingAt(beat, engine, world, 100.0f);

    SECTION("when the quest has come to rest")
    {
        engine.courier.questStage = 0;

        SECTION("should finish and forget the visit")
        {
            REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);
            REQUIRE(VisitState::GetSnapshot().senderFormID == 0);
        }
    }

    SECTION("when the quest is still mid-visit")
    {
        engine.courier.questStage = static_cast<std::uint16_t>(kStageDiscuss);
        engine.papyrus.packedInts.clear();

        SECTION("should force it into rollback rather than leave it running")
        {
            // Cleanup can be reached from a COMPOSE failure that never set a
            // terminal stage. Leaving the quest at Discuss would leave the
            // alias filled and the sender designated for the rest of the save.
            Tick(beat, BeatState::CLEANUP);
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageRollback)));
        }
    }

    SECTION("when the beat is taken away mid-visit")
    {
        engine.courier.questStage = static_cast<std::uint16_t>(kStageDiscuss);
        AbortFromMainThread(beat);

        SECTION("should send the sender home")
        {
            REQUIRE_FALSE(engine.questControl.teleports.empty());
            REQUIRE(engine.questControl.teleports.back().destinationFormID == kReturnAnchor);
        }

        SECTION("should shut the quest down")
        {
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageShutdown)));
        }

        SECTION("should record the visit as aborted")
        {
            const auto history = VisitState::GetHistory();
            REQUIRE_FALSE(history.empty());
            REQUIRE(history.front().outcome == VisitState::Outcome::Aborted);
        }

        SECTION("should not act twice if it is taken away again")
        {
            // Shutdown and revert can both reach Abort. A second teleport
            // would move a sender who is already home.
            const auto teleports = engine.questControl.teleports.size();
            AbortFromMainThread(beat);
            REQUIRE(engine.questControl.teleports.size() == teleports);
        }
    }
}

TEST_CASE("NPCVisitBeat remembers who has called", "[NPCVisitBeat][engine]")
{
    // Two ledgers with different jobs. The cooldown decays and stops the same
    // person calling twice in a week; the watermark never decays and stops
    // them calling twice about the same thing.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    engine.calendar.hoursPassed = 100.0f;

    SECTION("when a visit is delivered")
    {
        REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
        Cooldowns::OnVisitCompleted(kSender);

        SECTION("should take the sender off the candidate list")
        {
            REQUIRE(Cooldowns::IsSenderOnCooldown(kSender));
        }

        SECTION("should not mark their memories yet")
        {
            // Arrival and landing are separate moments. A visit that arrives
            // and is then aborted should not have consumed the topic.
            REQUIRE_FALSE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender).has_value());
        }
    }

    SECTION("when a visit reaches its closing")
    {
        Cooldowns::OnVisitReachedValediction(kSender);

        SECTION("should mark how far into their memories it read")
        {
            REQUIRE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender) == 100.0);
        }
    }

    SECTION("when the cooldown has run out")
    {
        Cooldowns::OnVisitCompleted(kSender);
        Cooldowns::OnVisitReachedValediction(kSender);
        engine.calendar.hoursPassed = 300.0f;

        SECTION("should offer the sender again")
        {
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
        }

        SECTION("should keep the memory watermark anyway")
        {
            REQUIRE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender) == 100.0);
        }
    }

    SECTION("when nobody is named")
    {
        SECTION("should record nothing")
        {
            // A visit whose sender failed to resolve. Stamping zero would put
            // a real form on cooldown the first time one hashed there.
            Cooldowns::OnVisitCompleted(0);
            Cooldowns::OnVisitReachedValediction(0);
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(0));
            REQUIRE_FALSE(Cooldowns::GetSenderMemoryWatermarkGameHours(0).has_value());
        }
    }
}

TEST_CASE("NPCVisitBeat carries its ledgers across a save", "[NPCVisitBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    engine.calendar.hoursPassed = 100.0f;
    Cooldowns::OnVisitCompleted(kSender);
    Cooldowns::OnVisitReachedValediction(kSender);

    SECTION("when the ledgers are written and read back")
    {
        // Both are keyed by FormID, and the ids in a co-save are the ids of
        // the load order that wrote it -- so both come back through the
        // migration table rather than as they were written.
        Persistence::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        Persistence::OnRevert();
        Persistence::OnLoad(FakeInterface(), kRecordVersion, 0);
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

    SECTION("when the save predates the watermark table")
    {
        // A v1 record carries the cooldowns and stops. The reader must not go
        // looking for bytes that were never written.
        Persistence::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        Persistence::OnRevert();
        Persistence::OnLoad(FakeInterface(), 1, 0);

        SECTION("should restore the cooldowns")
        {
            REQUIRE(Cooldowns::IsSenderOnCooldown(engine.cosave.resolvedFormID));
        }

        SECTION("should start the watermarks empty")
        {
            REQUIRE_FALSE(Cooldowns::GetSenderMemoryWatermarkGameHours(engine.cosave.resolvedFormID).has_value());
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        Persistence::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        Persistence::OnLoad(FakeInterface(), 99, 0);

        SECTION("should restore nothing")
        {
            // Reverting rather than keeping what was live: the live table
            // belongs to the save being left, not the one being entered.
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(engine.cosave.resolvedFormID));
        }
    }

    SECTION("when the record ends early")
    {
        engine.cosave.readable.clear();
        engine.cosave.readCursor = 0;

        SECTION("should clear rather than keep half a table")
        {
            Persistence::OnLoad(FakeInterface(), kRecordVersion, 0);
            REQUIRE_FALSE(Cooldowns::IsSenderOnCooldown(kSender));
            REQUIRE_FALSE(Cooldowns::GetSenderMemoryWatermarkGameHours(kSender).has_value());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should write nothing")
        {
            Persistence::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
        }

        SECTION("should leave what is live alone")
        {
            Persistence::OnLoad(nullptr, kRecordVersion, 0);
            REQUIRE(Cooldowns::IsSenderOnCooldown(kSender));
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            Persistence::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}

TEST_CASE("NPCVisitBeat without the forms it needs", "[NPCVisitBeat][engine]")
{
    // Its own case because the resolution runs once per process: a world that
    // never had the quest has to be the only world this process sees. This is
    // what a botched install of the mod's own ESP looks like.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    Init::Initialize();
    const NPCVisitBeat beat;

    SECTION("should never offer itself")
    {
        // Everything else is in place -- somebody to send, memories to send
        // them about -- and the beat still has to stay out of the manifest,
        // because there is no quest to warp anybody with.
        REQUIRE_FALSE(beat.IsAvailable(BeatContext{}));
    }
}

TEST_CASE("NPCVisitBeat with a quest missing its aliases", "[NPCVisitBeat][engine]")
{
    // The quest exists and the aliases on it do not, which is an ESP a patch
    // has edited the alias records out of.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    PrimeModel();
    BuildWorld(engine, /*withAliases=*/false);
    Init::Initialize();
    const NPCVisitBeat beat;

    SECTION("should never offer itself")
    {
        REQUIRE_FALSE(beat.IsAvailable(BeatContext{}));
    }
}
