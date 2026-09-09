#include <AmbushBeat.h>

#include <ConfiguredAttackerGroups.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <AmbushAttackerGroups.h>
#include <IBeat.h>
#include <LocationKeywords.h>
#include <MainThread.h>
#include <PluginThread.h>
#include <StuckRecovery.h>

#include <nlohmann/json.hpp>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Tests for the beat that has somebody jump the player on the road.
//
// It is the only beat that creates things: it finds ground the player cannot
// see, resolves a leveled list into a concrete NPC, places one reference per
// attacker, moves each to its point in the same frame, fills eight quest
// aliases through the Papyrus VM, waits for everyone to land, and only then
// arms the group. Every step of that can half-happen, and a half-happened
// ambush leaves permanent references standing in the world.
//
// So most of these cases are about the failure paths, and specifically about
// one property: whatever went wrong, nothing the beat created is still there
// afterwards. The exception is corpses, which are deliberately left for the
// player to loot and for the engine's own cell reset to reap.
//
// Mocked-engine throughout. The world is the same flat plane the spawn search
// is tested against -- ground at zero, navmesh over all of it, every camera
// line blocked so cover exists everywhere -- because what this beat is
// responsible for is what it does with the points, not which ones it picks.
//
// Two steps have no Papyrus behind them here and are stood in for by the test:
// the alias fill, which production dispatches as a VM call, and the stage
// changes, which are recorded rather than applied. Both are named where they
// happen.

namespace
{
    namespace Groups = NarrativeEngine::AmbushAttackerGroups;
    namespace Init = NarrativeEngine::AmbushBeat_Init;
    namespace Persistence = NarrativeEngine::AmbushBeat_Persistence;
    namespace PluginThread = NarrativeEngine::PluginThread;
    namespace StuckRecovery = NarrativeEngine::StuckRecovery;
    using NarrativeEngine::AmbushBeat;
    using NarrativeEngine::BeatContext;
    using NarrativeEngine::BeatPolarity;
    using NarrativeEngine::BeatState;
    using NarrativeEngine::TickMode;
    using NarrativeEngine::TickResult;
    using NarrativeEngine::Testing::ConfiguredAttackerGroups;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    // A one-second poll makes every accumulator in the beat a whole number of
    // ticks: it has no wall clock of its own and counts the master poll's
    // interval instead.
    constexpr const char* kSettings = "[General]\nbDebugMode=0\n"
                                      "[BeatSystem]\niBeatSystemPollIntervalMs=1000\n"
                                      "[Beats]\niAmbushDefaultAttackerCount=3\n"
                                      "iAmbushMinAttackerCount=2\niAmbushMaxAttackerCount=6\n"
                                      "iAmbushMinSpawnDistanceUnits=2500\niAmbushMaxSpawnDistanceUnits=5500\n"
                                      "iAmbushDefaultSpawnDistanceUnits=2500\n"
                                      "iAmbushEngageDistanceUnits=1500\niAmbushAbandonDistanceUnits=6000\n"
                                      "iAmbushMaxDurationSeconds=10\niAmbushPerBeatCooldownGameHours=24\n";

    constexpr const char* kBanditGroup = "[Group:bandits]\nDisplayName = Bandits\nLineForm = LCharBandit\n";

    constexpr std::uint32_t kPlayer = 0x00000014u;
    constexpr std::uint32_t kAmbushQuest = 0x0E020000u;
    constexpr std::uint32_t kBanditList = 0x0E020101u;
    constexpr std::uint32_t kBanditNPC = 0x0E020201u;
    constexpr std::uint32_t kGroundMesh = 0x0E020301u;
    constexpr float kWorldEdge = 9000.0f;

    constexpr std::uint32_t kStageEngaged = 20;
    constexpr std::uint32_t kStageComplete = 200;

    constexpr std::uint32_t kRecordVersion = 2;

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    struct AmbushWorld
    {
        EngineMock::AliasedQuest quest;
        RE::TESLevCharacter* banditList = nullptr;
        RE::TESNPC* bandit = nullptr;

        // The eight attacker aliases, in slot order. The PlayerRef alias sits
        // in front of them on the quest and is not one of these.
        std::vector<RE::BGSRefAlias*> attackers;
    };

    // The quest's alias layout, in the order the ESP declares it: PlayerRef
    // first, then Attacker01 through Attacker08. The beat parses the slot out
    // of the name, so the names are the contract rather than the order.
    std::vector<std::string> AliasNames(int attackerCount = 8)
    {
        std::vector<std::string> names{"PlayerRef"};
        for (int i = 1; i <= attackerCount; ++i) {
            names.push_back(i < 10 ? "Attacker0" + std::to_string(i) : "Attacker" + std::to_string(i));
        }
        return names;
    }

    AmbushWorld BuildWorld(EngineMock& engine, const std::vector<std::string>& aliases = AliasNames())
    {
        AmbushWorld world;
        EngineMock::QuestState state;
        state.editorID = "_ne_AmbushQuest";
        state.formID = kAmbushQuest;
        state.running = false;
        world.quest = engine.AddQuestWithAliases(state, aliases);
        for (std::size_t i = 1; i < world.quest.aliases.size(); ++i) {
            world.attackers.push_back(world.quest.aliases[i]);
        }

        // The player at the origin facing north, on flat dry navmeshed ground
        // with every camera line blocked -- the world the spawn search is
        // itself tested against. Positioned through the mock's own fields
        // rather than as a fabricated actor, because everything here reaches
        // the player through PlayerCharacter::GetSingleton.
        engine.world.playerX = 0.0f;
        engine.world.playerY = 0.0f;
        engine.world.playerZ = 0.0f;
        engine.world.cellIsInterior = false;
        engine.terrain.landHeight = 0.0f;
        engine.AddNavmeshPatch(
            engine.GroundCell(), kGroundMesh, -kWorldEdge, -kWorldEdge, kWorldEdge, kWorldEdge, 0.0f);
        engine.visibility.pickHitFraction = 0.0f;

        // Every keyword the location gate reads, registered before anything
        // asks: the module resolves each list once per process, so a keyword
        // added after the first question stays unresolved for the rest of it.
        for (const auto edid : NarrativeEngine::LocationKeywords::kSafe) {
            (void)engine.AddKeyword(edid);
        }
        for (const auto edid : NarrativeEngine::LocationKeywords::kDangerous) {
            (void)engine.AddKeyword(edid);
        }
        for (const auto edid : NarrativeEngine::LocationKeywords::kOccupied) {
            (void)engine.AddKeyword(edid);
        }

        world.banditList = engine.AddLeveledCharacter(kBanditList, "LCharBandit");
        world.bandit = engine.AddNPC(kBanditNPC, "Bandit");
        engine.SetLeveledResult(world.banditList, world.bandit);
        return world;
    }

    TickResult Tick(AmbushBeat& beat, BeatState state, TickMode mode = TickMode::Normal)
    {
        TickResult result;
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { result = beat.Tick(pt, mode, state); });
        return result;
    }

    // Stand in for the one step the harness has no Papyrus for: the quest
    // script's FillAttackerSlot, which production dispatches fire-and-forget
    // and then reads back a tick later.
    void FillSpawnedAliases(EngineMock& engine, const AmbushWorld& world)
    {
        for (std::size_t i = 0; i < engine.spawn.placed.size() && i < world.attackers.size(); ++i) {
            auto* form = RE::TESForm::LookupByID(engine.spawn.placed[i].refFormID);
            engine.FillRefAlias(world.attackers[i], form ? form->AsReference() : nullptr);
        }
    }

    // Drive COMPOSE to its conclusion, filling the aliases as the VM would.
    // Every gate inside it counts poll intervals rather than wall clock, so
    // this terminates in a bounded number of ticks.
    BeatState RunCompose(AmbushBeat& beat, EngineMock& engine, const AmbushWorld& world, bool fillAliases = true)
    {
        for (int i = 0; i < 60; ++i) {
            if (fillAliases) {
                FillSpawnedAliases(engine, world);
            }
            if (const auto result = Tick(beat, BeatState::COMPOSE); result.transitionTo) {
                return *result.transitionTo;
            }
        }
        return BeatState::COMPOSE;
    }

    // Run RUNNING until it asks to leave, or give up. Bounded by the beat's
    // own abandon timeout.
    BeatState RunUntilDone(AmbushBeat& beat, int maxTicks = 40)
    {
        for (int i = 0; i < maxTicks; ++i) {
            if (const auto result = Tick(beat, BeatState::RUNNING); result.transitionTo) {
                return *result.transitionTo;
            }
        }
        return BeatState::RUNNING;
    }

    void AbortFromMainThread(AmbushBeat& beat)
    {
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            NarrativeEngine::MainThread::Run(pt, [&](const NarrativeEngine::MainThread::Token& mt) { beat.Abort(mt); });
        });
    }

    std::size_t DispatchesOf(const EngineMock& engine, const std::string& method)
    {
        return static_cast<std::size_t>(std::count_if(engine.papyrus.dispatches.begin(),
                                                      engine.papyrus.dispatches.end(),
                                                      [&](const auto& sent) { return sent.methodName == method; }));
    }

    bool DispatchedStage(const EngineMock& engine, std::int32_t stage)
    {
        return std::find(engine.papyrus.packedInts.begin(), engine.papyrus.packedInts.end(), stage)
               != engine.papyrus.packedInts.end();
    }

    // Every reference the beat placed that is still in the world.
    std::size_t SurvivingRefs(const EngineMock& engine)
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
} // namespace

TEST_CASE("AmbushBeat says what it is", "[AmbushBeat][engine]")
{
    const AmbushBeat beat;

    SECTION("should answer to the name the Director picks it by")
    {
        REQUIRE(beat.Name() == "ambush");
    }

    SECTION("should only ever raise tension")
    {
        // Unlike the social beats. Nothing about being jumped on the road
        // lowers the temperature, so the Director must not reach for it when
        // it wants things to calm down.
        REQUIRE(beat.Polarity() == BeatPolarity::Raise);
    }

    SECTION("should ask the model for the two things it uses")
    {
        // The description IS the prompt the picker reads. The group decides
        // who turns up and the prose is the only in-fiction account of why.
        REQUIRE(beat.Description().find("attacker_group") != std::string::npos);
        REQUIRE(beat.Description().find("narration_prose") != std::string::npos);
    }
}

TEST_CASE("AmbushBeat says when it can be offered", "[AmbushBeat][engine]")
{
    // Happy path, re-run per leaf: the quest resolved with its full alias
    // layout, one always-eligible group loaded, and a player standing outdoors
    // somewhere unremarkable.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    const AmbushBeat beat;
    BeatContext ctx;

    SECTION("when the road is clear")
    {
        SECTION("should offer itself")
        {
            REQUIRE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the player is indoors")
    {
        ctx.playerInInterior = true;

        SECTION("should stay out of it")
        {
            // The beat spawns a group that walks in from a distance. An
            // interior has neither the room nor the sightlines for that.
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the player is somewhere people already live")
    {
        // Guards and scheduled NPCs already own those cells; a road ambush
        // dropped into one is three bandits materialising in a marketplace.
        auto* town = engine.AddLocation(0x0E020401u, "Whiterun", {"LocTypeCity"});
        engine.world.playerLocationOverride = town;

        SECTION("should stay out of it")
        {
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when vanilla has already marked the place as busy")
    {
        // A garrisoned fort is both occupied and dangerous, and occupied has
        // to win -- so the occupied test cannot sit behind any other keyword.
        auto* fort = engine.AddLocation(0x0E020403u, "Fort Greymoor", {"CWEventHappening", "LocTypeBanditCamp"});
        engine.world.playerLocationOverride = fort;

        SECTION("should stay out of it")
        {
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when the player is somewhere already full of enemies")
    {
        // Vanilla populates dungeons and camps itself; adding a road ambush
        // on top of one stacks two encounters into a gauntlet.
        auto* den = engine.AddLocation(0x0E020402u, "Bleak Falls Barrow", {"LocTypeBanditCamp"});
        engine.world.playerLocationOverride = den;

        SECTION("should stay out of it")
        {
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }

    SECTION("when no attacker groups loaded")
    {
        const ConfiguredAttackerGroups none{"; nothing here\n"};

        SECTION("should stay out of it")
        {
            // The group file is content and can be missing, empty, or entirely
            // rejected by the parser. There is nothing to spawn in any of
            // those cases and no way to find out later.
            REQUIRE_FALSE(beat.IsAvailable(ctx));
        }
    }
}

TEST_CASE("AmbushBeat puts a group in the player's path", "[AmbushBeat][engine]")
{
    // The COMPOSE walk, end to end. Every step of it creates something in the
    // world, so this is also the reference for what a successful ambush leaves
    // behind.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    AmbushBeat beat;
    nlohmann::json params;
    params["attacker_group"] = "bandits";
    params["narration_prose"] = "Three of them step out of the treeline with their hoods up.";
    beat.OnStart(BeatContext{}, params);
    const auto next = RunCompose(beat, engine, world);

    SECTION("should go on to run the encounter")
    {
        REQUIRE(next == BeatState::RUNNING);
    }

    SECTION("should place one attacker per body it was asked for")
    {
        REQUIRE(engine.spawn.placed.size() == 3);
    }

    SECTION("should place them from a concrete NPC rather than the list itself")
    {
        REQUIRE(engine.spawn.placed.size() == 3);
        // A reference made from a leveled list type-checks and produces an
        // unresolved placeholder that never becomes an Actor -- so the alias
        // fills, nothing arms, and the first poll reads every slot as gone.
        REQUIRE(engine.spawn.placed.front().base == static_cast<const void*>(world.bandit));
    }

    SECTION("should move them off the player before anything is rendered")
    {
        // They are created at the player's own position, because that is
        // where PlaceAtMe puts them. Left there, the player watches three
        // bandits appear inside their own body.
        REQUIRE(engine.spawn.placed.size() == 3);
        for (const auto& placement : engine.spawn.placed) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(placement.refFormID);
            REQUIRE(actor != nullptr);
            const auto at = actor->GetPosition();
            REQUIRE(std::sqrt(at.x * at.x + at.y * at.y) > 1000.0f);
        }
    }

    SECTION("should ask the quest to take each one into a slot")
    {
        // ForceRefTo has no native binding, so the fill is the one part of
        // the spawn that has to go out through the VM.
        REQUIRE(DispatchesOf(engine, "FillAttackerSlot") == 3);
    }

    SECTION("should leave them unwilling to fight on their own initiative")
    {
        // An attacker carries its base aggression from the moment it exists.
        // Anything above zero and the group starts a fight from wherever it
        // spawned, which is out of sight and out of range.
        REQUIRE_FALSE(engine.actorValues.setCalls.empty());
        for (const auto& call : engine.actorValues.setCalls) {
            REQUIRE(call.value == 0.0f);
        }
    }

    SECTION("should move the quest to the stage that starts them walking")
    {
        REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageEngaged)));
    }

    SECTION("should put the group it used on its own cooldown")
    {
        // Stamped here rather than at OnStart, because this is the first
        // moment attackers demonstrably exist. A compose that failed earlier
        // must not retire the group it was going to use.
        REQUIRE(Groups::RemainingCooldownGameHours("bandits") > 0.0);
    }
}

TEST_CASE("AmbushBeat gives up rather than half-stage an ambush", "[AmbushBeat][engine]")
{
    // Every way the staging can fail after the Director has asked for it. All
    // of them share one requirement: nothing the beat created is still
    // standing in the world afterwards.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    AmbushBeat beat;
    beat.OnStart(BeatContext{}, nlohmann::json::object());

    SECTION("when the player walked indoors while the Director was thinking")
    {
        // The beat-select round trip takes seconds, which is long enough to
        // walk into a cave. Caught before anything is spawned.
        engine.world.cellIsInterior = true;

        SECTION("should fail before creating anything")
        {
            REQUIRE(RunCompose(beat, engine, world) == BeatState::CLEANUP);
            REQUIRE(engine.spawn.placed.empty());
        }
    }

    SECTION("when there is nowhere to put anybody")
    {
        // No ground resolves anywhere, so every sampled point fails the
        // first gate the search puts it through.
        engine.terrain.landHeightResolves = false;

        SECTION("should fail before creating anything")
        {
            REQUIRE(RunCompose(beat, engine, world) == BeatState::CLEANUP);
            REQUIRE(engine.spawn.placed.empty());
        }
    }

    SECTION("when the quest will not start")
    {
        engine.questControl.startResult = false;

        SECTION("should fail before creating anything")
        {
            REQUIRE(RunCompose(beat, engine, world) == BeatState::CLEANUP);
            REQUIRE(engine.spawn.placed.empty());
        }
    }

    SECTION("when the leveled list resolves to nobody")
    {
        // A list whose entries are all above the player's level, or one a mod
        // emptied. Nothing to place.
        engine.SetLeveledResult(world.banditList, nullptr);

        SECTION("should fail before creating anything")
        {
            REQUIRE(RunCompose(beat, engine, world) == BeatState::CLEANUP);
            REQUIRE(engine.spawn.placed.empty());
        }
    }

    SECTION("when the engine refuses to place the second attacker")
    {
        // A partial spawn, which is the case this whole teardown path exists
        // for: failing at attacker two of three would otherwise strand the
        // first one on the road forever.
        engine.spawn.placeSucceedsCount = 1;

        SECTION("should take back the one it did place")
        {
            REQUIRE(RunCompose(beat, engine, world) == BeatState::CLEANUP);
            REQUIRE(engine.spawn.placed.size() == 1);
            REQUIRE(SurvivingRefs(engine) == 0);
        }
    }

    SECTION("when the quest never takes the attackers into its slots")
    {
        SECTION("should take them all back")
        {
            // The fill is fire-and-forget through the VM, so the only way to
            // find out it never happened is to stop waiting.
            REQUIRE(RunCompose(beat, engine, world, /*fillAliases=*/false) == BeatState::CLEANUP);
            REQUIRE(engine.spawn.placed.size() == 3);
            REQUIRE(SurvivingRefs(engine) == 0);
        }
    }

    SECTION("when what landed in the slots are not actors")
    {
        SECTION("should take them all back")
        {
            // Filled but useless: nothing would arm, and the first poll would
            // read every slot as already dead.
            auto* prop = engine.AddReference(nullptr, 0x0E020501u, {});
            for (int i = 0; i < 60; ++i) {
                for (auto* alias : world.attackers) {
                    engine.FillRefAlias(alias, prop);
                }
                if (const auto result = Tick(beat, BeatState::COMPOSE); result.transitionTo) {
                    REQUIRE(*result.transitionTo == BeatState::CLEANUP);
                    break;
                }
            }
            REQUIRE(SurvivingRefs(engine) == 0);
        }
    }

    SECTION("when everybody lands in water")
    {
        // Positive evidence the pre-spawn gates cannot give: a shoreline or a
        // shallow river passes every height and navmesh check and still puts
        // three bandits swimming at the player.
        engine.spawn.placedInWater = true;

        SECTION("should take them all back")
        {
            REQUIRE(RunCompose(beat, engine, world) == BeatState::CLEANUP);
            REQUIRE(SurvivingRefs(engine) == 0);
        }
    }
}

TEST_CASE("AmbushBeat turns the group hostile together", "[AmbushBeat][engine]")
{
    // The handoff from travelling to fighting. The group arrives strung out,
    // so it is all-or-nothing on the first arrival rather than per attacker.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    AmbushBeat beat;
    beat.OnStart(BeatContext{}, nlohmann::json::object());
    REQUIRE(RunCompose(beat, engine, world) == BeatState::RUNNING);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageEngaged);
    engine.papyrus.dispatches.clear();
    engine.actorValues.setCalls.clear();

    SECTION("when they are all still walking in")
    {
        SECTION("should leave them peaceable")
        {
            Tick(beat, BeatState::RUNNING);
            REQUIRE(DispatchesOf(engine, "EngageAttacker") == 0);
        }
    }

    SECTION("when one of them closes on the player")
    {
        auto* first = RE::TESForm::LookupByID<RE::Actor>(engine.spawn.placed.front().refFormID);
        first->data.location = RE::NiPoint3{100.0f, 0.0f, 0.0f};
        Tick(beat, BeatState::RUNNING);

        SECTION("should set the whole group on the player")
        {
            // Not just the one who arrived: the stragglers would otherwise
            // walk into a fight that had already started, at aggression zero.
            REQUIRE(DispatchesOf(engine, "EngageAttacker") == 3);
        }

        SECTION("should make them willing to fight")
        {
            REQUIRE_FALSE(engine.actorValues.setCalls.empty());
            for (const auto& call : engine.actorValues.setCalls) {
                REQUIRE(call.value == 2.0f);
            }
        }

        SECTION("should not do it twice")
        {
            // A latch rather than a distance test each poll, or every tick
            // for the rest of the fight re-sets aggression on the survivors.
            Tick(beat, BeatState::RUNNING);
            REQUIRE(DispatchesOf(engine, "EngageAttacker") == 3);
        }
    }
}

TEST_CASE("AmbushBeat says who is jumping the player", "[AmbushBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    AmbushBeat beat;
    nlohmann::json params;
    params["narration_prose"] = "Three of them step out of the treeline with their hoods up.";
    beat.OnStart(BeatContext{}, params);
    REQUIRE(RunCompose(beat, engine, world) == BeatState::RUNNING);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageEngaged);
    engine.papyrus.dispatches.clear();

    SECTION("when the fight has not started")
    {
        SECTION("should hold the prose back")
        {
            // Submitting it at spawn would put "three of them step out of the
            // treeline" in the log while the group is still over the hill.
            Tick(beat, BeatState::RUNNING);
            REQUIRE(DispatchesOf(engine, "RunAmbushNarration") == 0);
        }
    }

    SECTION("when the player is actually fighting")
    {
        engine.player.inCombat = true;
        Tick(beat, BeatState::RUNNING);

        SECTION("should say it once")
        {
            REQUIRE(DispatchesOf(engine, "RunAmbushNarration") == 1);
        }

        SECTION("should not say it again when the fight breaks off and resumes")
        {
            engine.player.inCombat = false;
            Tick(beat, BeatState::RUNNING);
            engine.player.inCombat = true;
            Tick(beat, BeatState::RUNNING);
            REQUIRE(DispatchesOf(engine, "RunAmbushNarration") == 1);
        }
    }

    SECTION("when the Director supplied no prose")
    {
        AmbushBeat quiet;
        quiet.OnStart(BeatContext{}, nlohmann::json::object());

        SECTION("should say nothing rather than an empty line")
        {
            engine.player.inCombat = true;
            Tick(quiet, BeatState::RUNNING);
            REQUIRE(DispatchesOf(engine, "RunAmbushNarration") == 0);
        }
    }
}

TEST_CASE("AmbushBeat ends the encounter", "[AmbushBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    AmbushBeat beat;
    beat.OnStart(BeatContext{}, nlohmann::json::object());
    REQUIRE(RunCompose(beat, engine, world) == BeatState::RUNNING);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageEngaged);

    SECTION("when the fight is still going")
    {
        SECTION("should let it run")
        {
            REQUIRE(RunUntilDone(beat, 4) == BeatState::RUNNING);
        }
    }

    SECTION("when every attacker is down")
    {
        engine.forms.actorIsDead = true;

        SECTION("should finish")
        {
            REQUIRE(RunUntilDone(beat) == BeatState::CLEANUP);
        }
    }

    SECTION("when the player outruns them")
    {
        for (const auto& placement : engine.spawn.placed) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(placement.refFormID);
            actor->data.location = RE::NiPoint3{20000.0f, 0.0f, 0.0f};
        }

        SECTION("should finish")
        {
            // Every survivor beyond the abandon distance, not merely some.
            // A player who rode away is not owed a fight that follows them.
            REQUIRE(RunUntilDone(beat) == BeatState::CLEANUP);
        }
    }

    SECTION("when nothing has resolved it either way")
    {
        SECTION("should give up on its own timeout")
        {
            // Ten seconds of poll intervals here. Without it, a group that
            // gets stuck out of reach holds the beat slot for the session.
            REQUIRE(RunUntilDone(beat, 30) == BeatState::CLEANUP);
        }
    }

    SECTION("when the save was restored with nothing behind it")
    {
        // BeatSystem restores RUNNING from the co-save, and a crash or a load
        // from before the encounter leaves no world to match. Nothing else
        // recovers this: the beat is still registered, so the unknown-beat
        // reset never fires.
        engine.courier.questStage = 0;

        SECTION("should recover rather than poll an empty encounter")
        {
            REQUIRE(Tick(beat, BeatState::RUNNING).transitionTo == BeatState::CLEANUP);
        }
    }
}

TEST_CASE("AmbushBeat clears up after itself", "[AmbushBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    engine.calendar.hoursPassed = 100.0f;
    AmbushBeat beat;
    beat.OnStart(BeatContext{}, nlohmann::json::object());
    REQUIRE(RunCompose(beat, engine, world) == BeatState::RUNNING);
    engine.courier.questStage = static_cast<std::uint16_t>(kStageEngaged);
    engine.papyrus.packedInts.clear();
    // COMPOSE opens by retiring whatever the previous encounter left, which
    // stops and resets the quest. Cleared so the records below are about the
    // teardown rather than about that.
    engine.questControl.stopped.clear();
    engine.questControl.reset.clear();

    SECTION("when the encounter ran")
    {
        REQUIRE(Tick(beat, BeatState::CLEANUP).transitionTo == BeatState::NOT_RUNNING);

        SECTION("should take the survivors away")
        {
            // They are permanent references standing on a road. Left behind,
            // every ambush the player ever gets adds three more.
            REQUIRE(SurvivingRefs(engine) == 0);
        }

        SECTION("should mark the quest complete without stopping it")
        {
            // Stopping it releases the aliases, and the engine then reaps the
            // corpses before the player can loot them. The stop is deferred
            // to the next encounter's own setup.
            REQUIRE(DispatchedStage(engine, static_cast<std::int32_t>(kStageComplete)));
            REQUIRE(engine.questControl.stopped.empty());
        }

        SECTION("should start the beat's cooldown")
        {
            REQUIRE(beat.RemainingCooldownGameHours() == 24.0);
        }
    }

    SECTION("when the attackers died in the fight")
    {
        engine.forms.actorIsDead = true;

        SECTION("should leave the bodies where they fell")
        {
            // They carry the encounter's loot, and deleting one out from
            // under a player mid-loot is worse than leaving a dynamic
            // reference for the cell reset to reap.
            Tick(beat, BeatState::CLEANUP);
            REQUIRE(SurvivingRefs(engine) == 3);
        }
    }

    SECTION("when the staging failed instead")
    {
        AmbushBeat failed;
        failed.OnStart(BeatContext{}, nlohmann::json::object());
        engine.world.cellIsInterior = true;
        REQUIRE(RunCompose(failed, engine, world) == BeatState::CLEANUP);
        Tick(failed, BeatState::CLEANUP);

        SECTION("should not burn a day on an ambush that never happened")
        {
            REQUIRE(failed.RemainingCooldownGameHours() == 0.0);
        }
    }

    SECTION("when the beat is taken away mid-encounter")
    {
        AbortFromMainThread(beat);

        SECTION("should leave nothing of it in the world")
        {
            // Abort's contract is stronger than cleanup's: corpses included,
            // because this is a shutdown or a revert and there will be no
            // next encounter to retire the quest.
            REQUIRE(SurvivingRefs(engine) == 0);
            REQUIRE(engine.questControl.stopped == std::vector<const void*>{world.quest.quest});
            REQUIRE(engine.questControl.reset == std::vector<const void*>{world.quest.quest});
        }

        SECTION("should not burn a day either")
        {
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }
    }
}

TEST_CASE("AmbushBeat counts down its own cooldown", "[AmbushBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    engine.calendar.hoursPassed = 100.0f;
    AmbushBeat beat;

    SECTION("when no ambush has ever run")
    {
        SECTION("should report nothing left to wait for")
        {
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }
    }

    SECTION("when one has just finished")
    {
        beat.OnStart(BeatContext{}, nlohmann::json::object());
        REQUIRE(RunCompose(beat, engine, world) == BeatState::RUNNING);
        Tick(beat, BeatState::CLEANUP);

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
            engine.calendar.hoursPassed = 130.0f;
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }

        SECTION("should report nothing when the cooldown is switched off")
        {
            const ConfiguredSettings uncapped{"[Beats]\niAmbushPerBeatCooldownGameHours=0\n"};
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }
    }
}

TEST_CASE("AmbushBeat carries its cooldowns across a save", "[AmbushBeat][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    Persistence::OnRevert();
    const auto world = BuildWorld(engine);
    // Loaded after the world, because the parser resolves every form a group
    // names at load time and skips a group whose leveled list is not there.
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Init::Initialize();
    engine.calendar.hoursPassed = 100.0f;
    AmbushBeat beat;
    beat.OnStart(BeatContext{}, nlohmann::json::object());
    REQUIRE(RunCompose(beat, engine, world) == BeatState::RUNNING);
    Tick(beat, BeatState::CLEANUP);

    SECTION("when the state is written and read back")
    {
        Persistence::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        Persistence::OnRevert();
        Persistence::OnLoad(FakeInterface(), kRecordVersion, 0);

        SECTION("should bring the beat's own clock back")
        {
            REQUIRE(beat.RemainingCooldownGameHours() == 24.0);
        }

        SECTION("should bring the per-group cooldowns back")
        {
            // They are beat state rather than config, so they ride in this
            // record even though they live with the group table.
            REQUIRE(Groups::RemainingCooldownGameHours("bandits") > 0.0);
        }
    }

    SECTION("when the save predates the per-group table")
    {
        Persistence::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        Persistence::OnRevert();
        Persistence::OnLoad(FakeInterface(), 1, 0);

        SECTION("should restore the beat's clock and leave every group ready")
        {
            // The safe direction for a one-time upgrade: a group that reads
            // as ready is offered again a little early, which is better than
            // one that reads as spent forever.
            REQUIRE(beat.RemainingCooldownGameHours() == 24.0);
            REQUIRE_FALSE(Groups::RemainingCooldownGameHours("bandits") > 0.0);
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
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }
    }

    SECTION("when the record ends before the clock does")
    {
        engine.cosave.readable.clear();
        engine.cosave.readCursor = 0;

        SECTION("should restore nothing")
        {
            Persistence::OnLoad(FakeInterface(), kRecordVersion, 0);
            REQUIRE(beat.RemainingCooldownGameHours() == 0.0);
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should write nothing")
        {
            engine.cosave.written.clear();
            Persistence::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
        }

        SECTION("should leave what is live alone")
        {
            Persistence::OnLoad(nullptr, kRecordVersion, 0);
            REQUIRE(beat.RemainingCooldownGameHours() == 24.0);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.written.clear();
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            Persistence::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}

TEST_CASE("AmbushBeat without its quest", "[AmbushBeat][engine]")
{
    // Its own case because the resolution runs once per process: a world that
    // never had the quest has to be the only world this process sees.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const ConfiguredAttackerGroups groups{kBanditGroup};
    Persistence::OnRevert();
    Init::Initialize();
    const AmbushBeat beat;

    SECTION("should never offer itself")
    {
        REQUIRE_FALSE(beat.IsAvailable(BeatContext{}));
    }
}
