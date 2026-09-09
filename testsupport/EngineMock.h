#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RE
{
    template <class Event> class BSTEventSource;
    class Actor;
    class BGSBaseAlias;
    class BGSKeyword;
    class BGSLocation;
    class NavMesh;
    class NavMeshInfoMap;
    class TESGlobal;
    class TESLevCharacter;
    class TESNPC;
    class TESRace;
    struct BSNavmeshInfo;
    class Sky;
    class TESFaction;
    class TESObjectCELL;
    class TESWorldSpace;
    class TESObjectREFR;
    class TESQuest;
    class Calendar;
    class PlayerCharacter;
    class ScriptEventSourceHolder;
    class UI;
} // namespace RE

namespace SKSE
{
    struct ModCallbackEvent;
}

// EngineMock — stands in for the CommonLibSSE functions production code calls,
// so a test can drive engine-coupled code with no Skyrim process.
//
// How this works, because it is not the usual mocking story:
//
// Most of CommonLibSSE is header-only — struct layouts, member offsets, and
// inline helpers all compile straight into our translation unit. Only some
// functions are out-of-line, and those live in CommonLibSSE.lib, where each one
// looks its address up in the loaded SkyrimSE.exe through the address library.
// Those are the functions a test process cannot run, and they are exactly the
// ones we replace.
//
// The mocked-engine test target therefore compiles production sources against
// the REAL CommonLibSSE headers and then simply does not link CommonLibSSE.lib.
// testsupport/EngineMock.cpp provides its own definitions for the engine
// functions instead, and the MSVC linker takes them. Nothing about production
// code changes — `EngineUtils.cpp` is compiled here byte-for-byte as the DLL
// compiles it, calls `RE::Calendar::GetSingleton()` exactly as it always did,
// and reaches our definition rather than Bethesda's.
//
// The linker is the safety net. Any engine function the code under test touches
// that nobody has mocked is an unresolved external and the build fails by name.
// There is no way to accidentally call into real engine code, and no way to
// silently miss a dependency.
//
// Usage — construct one, set what the engine should report, call the code:
//
//     EngineMock engine;
//     engine.calendar.hoursPassed = 42.5f;
//     REQUIRE(EngineUtils::GetCurrentGameHours() == 42.5);
//
// Construction installs the mock and destruction removes it, so ordinary scope
// is the reset. Only one may be alive at a time.
namespace NarrativeEngine::Testing
{
    // Which Skyrim build CommonLibSSE should believe it is running against.
    // This is not cosmetic: CommonLibSSE branches on it inside inline header
    // code, so it decides which of several real code paths production takes —
    // `ScriptEventSourceHolder::AsTESFastTravelEndEventSource()` returns null on
    // VR and a relocated member everywhere else, which is the entire reason
    // EngineUtils wraps it.
    enum class Runtime
    {
        SE,
        AE,
        VR
    };

    class EngineMock
    {
    public:
        explicit EngineMock(Runtime runtime = Runtime::AE);
        ~EngineMock();

        EngineMock(const EngineMock&) = delete;
        EngineMock& operator=(const EngineMock&) = delete;

        // `present == false` makes the corresponding GetSingleton() return
        // nullptr — the "engine subsystem isn't up yet" state that happens
        // during early plugin lifecycle and is otherwise impossible to
        // reproduce deliberately.
        struct CalendarState
        {
            bool present = true;
            float hoursPassed = 0.0f;
            int getHoursPassedCalls = 0;

            // The calendar reading the timestamp formatters render. Defaults
            // to the canonical game start, 17 Last Seed 4E 201, so a test that
            // does not set a date still gets a date that means something.
            float daysPassed = 0.0f;
            std::uint32_t year = 201;
            std::uint32_t month = 7; // Last Seed, 0-indexed
            float day = 17.0f;
            float hour = 12.0f;
        } calendar;

        struct UIState
        {
            bool present = true;
            bool gameIsPaused = false;

            // How many times anything has asked. Every cadenced loop in the
            // plugin checks this before doing anything else, so the count is
            // the cheapest exact measure of how often one of them ran.
            std::atomic<int> pausedQueries{0};
            std::vector<std::string> openMenus;
            std::vector<std::string> isMenuOpenQueries;
        } ui;

        struct PlayerState
        {
            bool present = true;
            bool inCombat = false;
        } player;

        struct EventSourceState
        {
            bool holderPresent = true;
        } events;

        // The Papyrus virtual machine. Anything that pokes a quest script goes
        // through here, and every failure in the chain — no VM, no handle
        // policy, a form the policy won't hand back a handle for, a VM that
        // refuses to queue the call — is a branch production code guards
        // against and a running game never shows you.
        struct PapyrusState
        {
            bool vmPresent = true;
            bool handlePolicyPresent = true;

            // What GetHandleForObject returns. Zero is what the policy gives
            // for a form it cannot make a handle for.
            std::uint64_t handle = 0x0000BEEF0000CAFEull;

            // What DispatchMethodCall returns: whether the VM accepted the
            // call onto its queue. It says nothing about the call later
            // succeeding, and neither does the code under test.
            bool dispatchSucceeds = true;

            struct HandleRequest
            {
                std::uint32_t formType = 0;
                const void* form = nullptr;
            };

            struct Dispatch
            {
                std::uint64_t handle = 0;
                std::string className;
                std::string methodName;
                bool hadArguments = false;
            };

            // Recorded interactions. Which script and method were named, and
            // with what arguments, is most of what correctness means here —
            // dispatching `Quest.SetStage` with the wrong stage number returns
            // true just as happily as the right one.
            std::vector<HandleRequest> handleRequests;
            std::vector<Dispatch> dispatches;
            std::vector<std::int32_t> packedInts;
            std::vector<bool> packedBools;
            // Strings packed as Papyrus arguments. A narration line reaches
            // the player through one of these and nothing else, so what was
            // packed is what was said.
            std::vector<std::string> packedStrings;
            // Forms packed as Papyrus object arguments, in the order they were
            // packed. A call that names the right method on the right script
            // and hands it the wrong reference is a call the VM accepts.
            std::vector<const void*> packedForms;
        } papyrus;

        // SKSE's co-save stream. Byte-accurate rather than value-accurate:
        // ReadRecordData returns a short count at the end of a record, which
        // is the whole shape of the failure handling that reads it.
        struct CosaveState
        {
            bool writeSucceeds = true;
            std::vector<std::byte> written;

            // Records opened, in order. A module that opens the wrong record
            // type or stamps the wrong version writes a payload no loader will
            // ever recognise, and nothing else about the save looks wrong.
            struct OpenedRecord
            {
                std::uint32_t type = 0;
                std::uint32_t version = 0;
            };

            bool openRecordSucceeds = true;
            std::vector<OpenedRecord> opened;

            std::vector<std::byte> readable;
            std::size_t readCursor = 0;

            bool resolveSucceeds = true;
            // What ResolveFormID hands back. Distinct from any id a test writes
            // so a passthrough would be visible.
            std::uint32_t resolvedFormID = 0x0BADF00Du;
            std::vector<std::uint32_t> resolveRequests;
        } cosave;

        // SKSE's ModEvent source, which Papyrus scripts send through. Backed by
        // a REAL BSTEventSource rather than opaque storage: AddEventSink and
        // SendEvent are both inline header code, so a genuine one lets a test
        // register a production sink and then dispatch to it exactly as the
        // Papyrus VM would — which is the only way to reach a sink that lives
        // in an anonymous namespace.
        // The world's terrain, water and navmesh, as far as placement code is
        // concerned. Deliberately a flat plane with a rectangular no-navmesh
        // patch rather than anything cleverer: what the code under test has to
        // get right is which points it ASKS about and what it does with a
        // refusal, not any real geometry.
        struct TerrainState
        {
            bool tesPresent = true;
            bool cellPresent = true;

            // Ground height everywhere, and whether it resolves at all. A
            // position over the void, or in an unloaded cell, has no ground.
            bool landHeightResolves = true;
            float landHeight = 0.0f;

            // Water surface. Below it is underwater; a cell with no water
            // reports none.
            bool hasWater = false;
            float waterHeight = 0.0f;

            // Navmesh is not answered from here. Containment is tested
            // against real triangles on the ground cell, so a test says where
            // an actor can stand by laying a patch with AddNavmeshPatch.
        } terrain;

        // The loaded exterior cell grid — the square of cells the streaming
        // system keeps around the player, and the only place navmesh geometry
        // can be read from.
        struct GridState
        {
            // TES::gridCells. Absent for the window between a load starting
            // and the first grid being built.
            bool present = true;
            // TES::interiorCell. Set means indoors, where there is no exterior
            // grid at all rather than an empty one.
            bool playerIndoors = false;
        } grid;

        // How the harness lays out the opaque BSNavmeshInfo block. The real one
        // on Skyrim SE 1.6 holds the navmesh FormID first and a position right
        // after it; a case that wants to know what happens when neither can be
        // found turns them off.
        struct NavInfoState
        {
            std::size_t formIDOffset = 0x00;
            bool writeFormID = true;

            std::size_t positionOffset = 0x04;
            bool writePosition = true;
        } navInfo;

        // What an actor did when placement code moved it.
        struct PlacementState
        {
            struct Move
            {
                std::uint32_t formID = 0;
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
            };

            std::vector<Move> moves;
            int packageEvaluations = 0;
            // Update3DPosition calls. Required alongside the position write, or
            // the physics body stays where it was and the actor walks straight
            // back into the geometry it was pulled out of.
            int warpUpdates = 0;

            // Where each tracked actor stands. Keyed by form id so several can
            // be escorted at once.
            std::map<std::uint32_t, Move> positions;

            // What TESObjectREFR::GetName answers for any actor.
            std::string actorName = "Bandit";
        } placement;

        // Quests, the aliases they fill actors into, and the scene an actor is
        // playing. What the sender-viability walk reads to decide whether some
        // other mod's authored content already owns an NPC.
        struct QuestState
        {
            bool stopped = false;
            bool completed = false;
            bool running = true;
            // The ESP a quest was authored in. Ours is self-excluded, so this
            // is what tells a foreign quest from one of our own.
            std::string sourceFile = "SomeOtherMod.esp";
            std::string editorID = "SomeOtherModQuest";
            std::uint32_t formID = 0x000AA001u;
        };

        struct AliasState
        {
            // Quests an actor is filled into, in the order the walk sees them.
            // Each entry is one (quest, alias) pair on the actor's extra data.
            struct Instance
            {
                QuestState quest;
                bool reserves = false;
                bool questObject = false;
                // Whether the alias contributes AI packages to the actor, which
                // is the engine's own record that it is being puppeteered.
                bool dispensesPackages = false;
            };

            // Absent means the actor carries no ExtraAliasInstanceArray at all,
            // which is the common case for an NPC nobody has filled.
            bool arrayPresent = false;
            std::vector<Instance> instances;

            // The quest that owns the scene the actor is playing, if any. Read
            // together with world.playerInScene.
            QuestState sceneQuest;
        } aliases;

        // What the player can see, as far as the visibility fan is concerned.
        // Modelled at the raycast rather than as geometry: the module's whole
        // question is "did a ray reach its endpoint", so one hit fraction says
        // everything a world of obstacles would.
        struct VisibilityState
        {
            bool cameraPresent = true;
            bool cameraRootPresent = true;
            float cameraX = 0.0f;
            float cameraY = 0.0f;
            float cameraZ = 0.0f;

            // Whether the target has 3D loaded, and how big its bound is. A
            // zero radius is a real state on freshly-attached 3D.
            bool target3DPresent = true;
            float targetBoundRadius = 64.0f;

            // What the engine's own line-of-sight call answers. Trusted as a
            // positive short-circuit only, so `false` here is not "invisible".
            bool engineLineOfSight = false;

            // What every raycast reports reaching. 1.0 is unobstructed; a low
            // fraction is geometry in the way.
            float pickHitFraction = 1.0f;
            int pickCalls = 0;

            // Skeleton nodes the fan looks for by name. Empty means none
            // resolve and the fan falls back to the bounding box, which is the
            // usual case for a non-actor reference.
            std::map<std::string, bool> namedNodes;
        } visibility;

        // The engine's high-process actor list. Anything that sweeps the
        // loaded actors walks this; the harness hands back whatever a test
        // registered with AddLoadedActor.
        struct ProcessListState
        {
            bool present = true;
        } processLists;

        // Magic, as far as anything watching combat needs it: whether the
        // spell that just landed was meant to hurt.
        struct MagicState
        {
            bool spellIsHostile = true;
        } magic;

        // Where SKSE says its log directory is. Modules that keep their own
        // trace file write there for real, so a test can read back exactly what
        // a player would send in with a bug report.
        struct LoggingState
        {
            bool directoryPresent = true;
            std::string directory = "Data/SKSE/Plugins/NarrativeEngineTestLogs";
        } logging;

        struct ModEventState
        {
            bool sourcePresent = true;
            // What TESForm::GetName answers, which the sink logs to say which
            // script sent an event.
            std::string senderName = "_ne_MCM";
        } modEvents;

        // The source itself. Not inside the state struct because it must
        // outlive any single EngineMock: a sink registered on it is a pointer
        // to a process-wide static that nothing ever unregisters.
        static RE::BSTEventSource<SKSE::ModCallbackEvent>& ModEventSource();

        // Put an actor into (or out of) bleedout. Written into the actor's own
        // life-state bitfield rather than answered from a mock flag, because
        // IsBleedingOut is inline: it reads the object, so a flag beside it
        // would be a value nothing consults.
        static void SetActorBleedingOut(RE::Actor* actor, bool bleedingOut);

        // Fabricate a worldspace the data handler will hand back when asked for
        // every WorldSpace form. Its cell map starts empty.
        //
        // The editor ID is optional and only matters where something names the
        // worldspace in a file or a log — worldspaces are otherwise identified
        // by their FormID.
        RE::TESWorldSpace* AddWorldSpace(std::uint32_t formID, std::string editorID = {});

        // Add an exterior cell to a worldspace's cell map at the given grid
        // coordinates, optionally belonging to a location.
        //
        // The map is constructed in place first, because a container sitting
        // in zeroed storage never got the non-zero end-of-chain sentinel its
        // own iterator tests against — without that it accepts inserts and
        // reports the right size while yielding nothing from a range-for.
        //
        // KNOWN LIMIT: even so, HoldGrid's builder still seeds nothing from a
        // world built this way — its LocTypeHold match fails on a location the
        // same keyword check accepts everywhere else, and that is not yet
        // diagnosed. HoldGrid is therefore not in the mocked-engine target and
        // has no tests. Everything else here is used and covered.
        RE::TESObjectCELL* AddExteriorCell(RE::TESWorldSpace* worldSpace,
                                           std::int16_t cellX,
                                           std::int16_t cellY,
                                           RE::BGSLocation* location);

        // Stand the player in an exterior cell of `worldSpace` at the given
        // grid coordinates, so a precomputed cell-to-hold grid can find them.
        void StandPlayerInCell(RE::TESWorldSpace* worldSpace, std::int16_t cellX, std::int16_t cellY);

        // Whether the streaming system has finished loading a cell. Cells sit
        // in the grid before their contents arrive, and code that reads a
        // cell's navmesh or references has to wait for this.
        void SetCellAttached(RE::TESObjectCELL* cell, bool attached);

        // Turn a fabricated cell into an interior one. Interiors reach the
        // loaded grid the same way exteriors do and have to be told apart
        // there, because most of what reads the grid is about the open world.
        void SetCellInterior(RE::TESObjectCELL* cell, bool interior);

        // One navmesh triangle, as a test describes it. Extraction reduces a
        // triangle to its centroid and to what lies across each of its three
        // edges, so that is the whole of what a test gets to say about one.
        struct FakeTriangle
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;

            // Road surface. The road graph is built from these and ignores
            // every other triangle in the mesh.
            bool preferred = true;
            // Retired by an edit but still present in the record.
            bool deleted = false;

            // Per edge: the index of another triangle in the SAME mesh, or -1
            // for an edge with nothing across it.
            std::array<int, 3> neighbor{-1, -1, -1};

            // Per edge: a portal across to another navmesh — the FormID of the
            // mesh and the triangle index within it. Zero mesh means no portal.
            std::array<std::uint32_t, 3> portalMesh{0, 0, 0};
            std::array<int, 3> portalTriangle{-1, -1, -1};

            // Per edge: an edge link that is not a portal. The record format
            // puts ledges in the same slot, and a dangling link is what a
            // corrupt or misread record looks like. Both are edges the graph
            // must decline to follow rather than trust.
            std::array<bool, 3> ledge{false, false, false};
            std::array<bool, 3> danglingLink{false, false, false};
        };

        // Give `cell` a navmesh built from these triangles. The mesh is added
        // to whatever the cell already has, so a cell can carry several.
        //
        // Each triangle gets three vertices of its own, all at the position
        // asked for, which puts the centroid there. A triangle asking for a
        // vertex index past the end of the array is a separate concern and is
        // reached with `AddNavMeshWithBadVertexIndex`.
        RE::NavMesh* AddNavMesh(RE::TESObjectCELL* cell,
                                std::uint32_t meshFormID,
                                const std::vector<FakeTriangle>& triangles);

        // A navmesh whose one road triangle names a vertex that does not
        // exist. Malformed data the extractor has to survive, and unreachable
        // through AddNavMesh because that one always writes matching vertices.
        RE::NavMesh* AddNavMeshWithBadVertexIndex(RE::TESObjectCELL* cell, std::uint32_t meshFormID);

        // Give `cell` an empty navmesh list, which is what an ocean or border
        // cell genuinely has. Distinct from a cell that was never given one:
        // that cell reports no list at all.
        void AddEmptyNavMeshList(RE::TESObjectCELL* cell);

        // Cover a rectangle of ground with navmesh, as the two triangles a
        // mesh generator would make of it. Containment is asked in the
        // horizontal plane and then checked against the surface height, so a
        // patch is the smallest honest way to say "an actor can stand
        // anywhere in here".
        RE::NavMesh* AddNavmeshPatch(RE::TESObjectCELL* cell,
                                     std::uint32_t meshFormID,
                                     float westX,
                                     float southY,
                                     float eastX,
                                     float northY,
                                     float surfaceZ);

        // The cell every world position resolves to. One cell stands in for
        // the whole exterior, because what code under test asks of it is what
        // is underfoot rather than which cell it is standing in.
        RE::TESObjectCELL* GroundCell();

        // Fabricate an NPC base form, which is what the gossip graph and every
        // relationship record are keyed on rather than the placed reference.
        // The display name is what anything rendering a line about this person
        // reads, so an NPC without one shows up as a blank in a sentence.
        RE::TESNPC* AddNPC(std::uint32_t formID, std::string name = {}, bool female = false);

        // Say which base form an actor was made from. Unique NPCs each have
        // their own; anything else in the world shares one with everybody of
        // its kind, which is why so much is keyed on the base rather than on
        // the placed reference.
        void SetActorBase(RE::Actor* actor, RE::TESNPC* base);

        // Give an actor a keyword. Actors inherit these from their base form
        // and their race in the engine; nothing here distinguishes the two,
        // because nothing that asks the question does either.
        void GiveActorKeyword(RE::Actor* actor, RE::BGSKeyword* keyword);

        // A levelled-character list, which is what content files name when
        // they mean "one of these" rather than a particular NPC.
        RE::TESLevCharacter* AddLeveledCharacter(std::uint32_t formID, std::string editorID);

        // A global variable holding whatever a test wants it to. Content files
        // gate on these and quest scripts write them, so their value is world
        // state rather than configuration.
        RE::TESGlobal* AddGlobal(std::uint32_t formID, std::string editorID, float value);

        // Declare a kinship between two NPCs, with the label the record would
        // carry. Labels are gendered and are read off the record rather than
        // invented, so both are given: `labelForMale` is what `a` calls `b`
        // when b is male, `labelForFemale` when b is female.
        //
        // Directional, as the engine's own record is: relationships name a
        // first and a second party, and which label applies depends on which
        // side the person being described sits.
        void AddRelationship(RE::TESNPC* a, RE::TESNPC* b, const char* labelForMale, const char* labelForFemale);

        // Put these cells in the loaded exterior grid, in row-major order over
        // the smallest square that fits them. Slots past the end stay null,
        // which is what a grid the streaming system has not filled looks like.
        void LoadGrid(const std::vector<RE::TESObjectCELL*>& cells);

        // Skyrim's one NAVI record: the precomputed long-distance pathing map.
        // Registered in the global form table, which is where it is found, and
        // kept for the life of the process because nothing takes a record down
        // inside a session.
        RE::NavMeshInfoMap* AddNavMeshInfoMap(std::uint32_t formID);

        // Tell the NAVI record about one navmesh, filed under the cell it
        // covers, and hand back the opaque handle the record identifies it by.
        //
        // That handle is a BSNavmeshInfo, which CommonLibSSE only
        // forward-declares — it has no member layout at all, so code that wants
        // the position inside one has to measure where it sits. The harness
        // therefore lays the block out itself, per `navInfo` below, and a case
        // that moves or omits a field is asking what happens when the
        // measurement fails.
        const RE::BSNavmeshInfo* AddNavmeshInfo(RE::NavMeshInfoMap* map,
                                                std::uint32_t navMeshFormID,
                                                std::uint32_t worldSpaceFormID,
                                                std::int16_t cellX,
                                                std::int16_t cellY,
                                                float x,
                                                float y,
                                                float z);

        // A resident NavMesh form with geometry bounds. Only a minority of
        // navmeshes have one loaded at any moment, and they are the only ones
        // whose position is known from the outside — which makes them the
        // samples any measurement of the opaque block is taken against.
        RE::NavMesh* AddNavMeshForm(std::uint32_t formID,
                                    RE::TESObjectCELL* parentCell,
                                    float minX,
                                    float minY,
                                    float maxX,
                                    float maxY);

        // One precomputed route: an ordered run of navmeshes the engine will
        // move a distant traveller along. Plotted end to end, these are the
        // roads.
        void AddPreferredPath(RE::NavMeshInfoMap* map, const std::vector<const RE::BSNavmeshInfo*>& chain);

        // Put a plain reference in a cell, where a sweep of the cell will find
        // it. Registered in the form table like any other form.
        RE::TESObjectREFR* AddReference(RE::TESObjectCELL* cell, std::uint32_t formID, RE::NiPoint3 position);

        // A load door standing in `cell` that comes out beside a door in
        // `destination`, at `arrival`. That arrival point is where an occupant
        // leaving the building actually appears, which is not where either door
        // itself stands.
        RE::TESObjectREFR* AddLoadDoor(RE::TESObjectCELL* cell,
                                       std::uint32_t formID,
                                       RE::TESObjectCELL* destination,
                                       RE::NiPoint3 arrival);

        // A door carrying teleport data that names no door on the other side,
        // which is what an unlinked or broken door looks like in a save.
        RE::TESObjectREFR* AddUnlinkedDoor(RE::TESObjectCELL* cell, std::uint32_t formID);

        // The map marker a location is pinned to. Coarser than a doorway: a
        // town's marker sits outside its walls.
        void SetLocationMarker(RE::BGSLocation* location, RE::TESObjectCELL* cell, RE::NiPoint3 position);

        // Fabricate a quest whose state the mocked TESQuest predicates answer
        // from, authored in the named ESP. Kept alive for the process: an alias
        // instance holds a bare pointer to it.
        RE::TESQuest* AddQuest(const QuestState& state);

        // A quest carrying named reference aliases, which is the shape a beat
        // drives: it starts the quest and then watches the Papyrus side fill
        // the aliases in. Every alias named here starts EMPTY rather than
        // falling back to the courier's single-alias answer, because the whole
        // of what the beat does while it waits is distinguish the two.
        //
        // Rebuilt at the SAME ADDRESS every time a given editor ID is asked
        // for, unlike everything else the harness fabricates. A module that
        // resolves its aliases at data load caches the pointers once per
        // process, and the next leaf of a sectioned test rebuilding the world
        // would otherwise leave that cache pointing at freed storage.
        struct AliasedQuest
        {
            RE::TESQuest* quest = nullptr;
            std::vector<RE::BGSRefAlias*> aliases; // in the order named
        };
        AliasedQuest AddQuestWithAliases(const QuestState& state, const std::vector<std::string>& aliasNames);

        // Fill a reference alias, as the quest's own Papyrus does once it
        // starts. Passing null empties it again.
        void FillRefAlias(RE::BGSRefAlias* alias, RE::TESObjectREFR* reference);

        // Fill `actor` into the aliases described by `aliases.instances`,
        // building the ExtraAliasInstanceArray the engine would have built.
        // Call after setting them up.
        void FillAliasInstances(RE::Actor* actor);

        // The console-command path: the engine's form factory, the transient
        // Script form it hands back, and what was compiled through it.
        struct ConsoleState
        {
            bool formFactoryPresent = true;
            // A factory that refuses to build a form. Rare, and essentially
            // only at shutdown, which is exactly why it never gets exercised.
            bool scriptCreationSucceeds = true;

            std::vector<std::string> commandsSet;
            std::vector<const void*> compileTargets;
        } console;

        // SKSE's main-thread task queue.
        struct TaskState
        {
            bool interfacePresent = true;

            // Queued tasks run inline, standing in for the main thread picking
            // them up on the next frame. Holding them instead would deadlock
            // MainThread::Run, which blocks on a future only the task
            // completes -- so deferral is opt-in and for FireAndForget only.
            bool runImmediately = true;

            int queued = 0;
            std::vector<std::function<void()>> held;
        } tasks;

        // The engine's form table, and the liveness flags the dispatch gates
        // read off an actor found in it.
        struct FormsState
        {
            bool actorIsDead = false;
            bool actorIsDisabled = false;
        } forms;

        // What containers and the player are carrying. Only counts are
        // modelled: the extra-data lists that carry per-reference state are
        // not fabricated, so code walking them finds a stack with none, which
        // is what an ordinary item looks like.
        struct InventoryState
        {
            // Keyed by the bound object's FormID.
            std::map<std::uint32_t, int> containerCounts;
            std::map<std::uint32_t, int> playerCounts;

            // Items taken out of somebody's inventory, in order. Which
            // container was asked is half of what matters: a sold letter is
            // in a merchant's chest and nowhere a sweep of actors would look.
            struct Removal
            {
                std::uint32_t holderFormID = 0;
                std::uint32_t itemFormID = 0;
                std::int32_t count = 0;
            };
            std::vector<Removal> removals;
        } inventory;

        // SKSE's own plumbing: the two interfaces a plugin asks for on the way
        // up, and the callbacks it hands them. Startup is the only place the
        // whole mod is wired together, and every one of those callbacks is
        // unreachable except through the registration -- so the harness keeps
        // them and a test calls them the way SKSE would.
        struct SKSEInterfaceState
        {
            bool messagingPresent = true;
            bool listenerRegisters = true;
            bool serializationPresent = true;

            int initCalls = 0;
            std::uint32_t uniqueID = 0;

            void (*messageListener)(SKSE::MessagingInterface::Message*) = nullptr;
            void (*saveCallback)(SKSE::SerializationInterface*) = nullptr;
            void (*loadCallback)(SKSE::SerializationInterface*) = nullptr;
            void (*revertCallback)(SKSE::SerializationInterface*) = nullptr;

            // What a load walk finds, in order. The three fields are all a
            // dispatcher has to route on, and an unknown type among them is
            // the case a co-save from another build produces.
            struct Record
            {
                std::uint32_t type = 0;
                std::uint32_t version = 1;
                std::uint32_t length = 0;
            };
            std::vector<Record> records;
            std::size_t recordCursor = 0;
        } skse;

        // The keyboard, as far as a hotkey sink is concerned. Present says the
        // input manager resolved at all; a sink that never registers is a
        // hotkey that never fires and says nothing about why.
        struct InputState
        {
            bool managerPresent = true;
        } input;

        // Actor values written through an actor's value owner. Aggression is
        // the one that matters here: it decides whether a spawned attacker
        // fights on its own initiative or waits to be told.
        struct ActorValueState
        {
            struct SetCall
            {
                std::uint32_t actorFormID = 0;
                int actorValue = 0;
                float value = 0.0f;
            };

            std::vector<SetCall> setCalls;
        } actorValues;

        // Putting a new reference into the world from a base object, which is
        // how an ambush's attackers arrive.
        struct SpawnState
        {
            // Whether the placement hands back a reference at all. The engine
            // answers with nothing when the base is unusable.
            bool placeSucceeds = true;

            // How many placements succeed before it starts refusing.
            // Negative means it never does. A partial spawn is the case the
            // teardown path exists for, and refusing the first one does not
            // reach it.
            int placeSucceedsCount = -1;

            // Whether a placed reference has a usable engine handle. The alias
            // fill rejects one that does not, and nothing downstream would say
            // why, so the beat checks it up front.
            bool refHandleIsValid = true;

            // Whether a placed actor reads as standing in water once it has
            // settled -- the positive evidence a pre-spawn height check
            // cannot give, and the reason a shoreline spawn is retried.
            bool placedInWater = false;

            struct Placement
            {
                const void* base = nullptr;
                std::uint32_t refFormID = 0;
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
            };

            // Every reference placed this session, in order, with where it
            // ended up. Placing an attacker and then failing to move them off
            // the player is the failure this records.
            std::vector<Placement> placed;

            // Where the next placed reference's FormID comes from.
            std::uint32_t nextRefFormID = 0x0F000001u;
        } spawn;

        // Starting, stopping and resetting a quest, which is how a beat drives
        // its own delivery quest through a run.
        struct QuestControlState
        {
            // EnsureQuestStarted has two answers and they fail independently:
            // whether the call itself went through, and what the engine
            // reported back through the out-parameter. Production checks both,
            // and a quest that reports failure both ways is a different world
            // from one that was simply never asked.
            bool startCallSucceeds = true;
            bool startResult = true;

            // Quests started, stopped and reset, in order. A rollback that
            // stops the wrong slot's quest leaves a letter in the world and
            // takes an unrelated one out of it.
            std::vector<const void*> started;
            std::vector<const void*> stopped;
            std::vector<const void*> reset;

            // References Disable() was called on, by FormID. Deleting a letter
            // is Disable followed by SetDelete, and the first is the half that
            // goes through the engine.
            std::vector<std::uint32_t> disabled;

            // Teleports, as (who moved, what they were moved to). Sending an
            // NPC home is one of these, and sending them to the wrong marker
            // leaves them standing wherever the visit ended.
            struct Teleport
            {
                std::uint32_t moverFormID = 0;
                std::uint32_t destinationFormID = 0;
            };
            std::vector<Teleport> teleports;
        } questControl;

        // The vanilla WICourier resolution: the quest, the container alias on
        // it, and the staging container's inventory.
        struct CourierState
        {
            bool questIsRunning = true;
            std::uint16_t questStage = 10;

            // What the Container alias's live reference resolves to. Null is a
            // mod that repointed the alias at nothing, which is the case the
            // fallback REFR exists for.
            bool aliasHasReference = true;

            // Absolute count the staging container reports for the one book a
            // test asks about. Negative means "the book is not in there at
            // all", which is a different answer from a count of zero.
            int inventoryCount = 3;
            int getInventoryCountsCalls = 0;
        } courier;

        // Build the vanilla WICourier quest, its `Container` alias, and the
        // `WICourierContainerRef` staging container, registering each under the
        // editor ID production looks it up by. Omit a piece to exercise the
        // resolution path that copes without it.
        RE::TESQuest* AddCourierQuest(bool withContainerAlias, bool withContainerRef);

        // Register a bound object (a book) in the form table under `formID`.
        // Register a bound object (a book) in the form table under `formID`,
        // optionally under an editor ID as well -- which is how a module that
        // owns a set of them resolves the set at data load.
        RE::TESForm* AddBook(std::uint32_t formID, std::string editorID = {});

        // Faction ranks, and the loaded-actor lists a sweep walks.
        //
        // Ranks are keyed by (actor form id, faction form id). An absent pair
        // means "not in the faction", which the engine reports as -1 and which
        // several guards below treat differently from rank 0.
        struct FactionState
        {
            std::map<std::pair<std::uint32_t, std::uint32_t>, int> ranks;

            struct AddToFactionCall
            {
                std::uint32_t actorFormID = 0;
                std::uint32_t factionFormID = 0;
                int rank = 0;
            };

            // Recorded because the sweep's correctness is about WHO it demotes
            // and to what, not about a return value it does not have.
            std::vector<AddToFactionCall> addToFactionCalls;
        } factions;

        // Put a stand-in Actor in the form table under `formID`. Each call
        // makes a distinct actor, so a test can tell two of them apart.
        RE::Actor* AddActor(std::uint32_t formID);

        // As AddActor, and also list the actor among ProcessLists' loaded
        // actors, which is what a faction sweep walks.
        RE::Actor* AddLoadedActor(std::uint32_t formID);

        // The placed reference AddResident fabricated for a unique NPC, which
        // is the actor anything asking whether that person is alive resolves.
        // Null for an NPC who was never made a resident of anywhere.
        RE::Actor* PlacedActorFor(std::uint32_t npcFormID);

        // What a leveled character list resolves to. The engine walks the
        // list's own flags and level filtering; the harness answers from what
        // a test declared, because what the code under test has to get right
        // is what it does with the answer -- including a list that resolves to
        // another list, and one that resolves to nothing.
        void SetLeveledResult(RE::TESLevCharacter* list, RE::TESForm* result);

        // A stand-in faction to hold ranks against.
        // The editor ID is optional and only matters when something names the
        // faction in a content file rather than holding a pointer to it.
        RE::TESFaction* AddFaction(std::uint32_t formID, std::string editorID = {});

        // Convenience over `factions.ranks`.
        void SetFactionRank(RE::Actor* actor, RE::TESFaction* faction, int rank);
        int FactionRank(RE::Actor* actor, RE::TESFaction* faction) const;

        // What the main-thread engine wrappers read back. These snapshots are
        // the only shape a worker thread ever sees of engine state, so what
        // matters is that each field arrives, not how the engine stores it.
        struct WorldState
        {
            std::uint32_t playerFormID = 0x00000014u;
            float playerX = 100.0f;
            float playerY = 200.0f;
            float playerZ = 300.0f;

            // Unmarked wilderness has no BGSLocation, which is a real and
            // common state rather than an error.
            bool playerHasLocation = true;
            std::uint32_t locationFormID = 0x00018A56u;
            std::string locationName = "Whiterun";

            bool playerHasCell = true;
            std::uint32_t cellFormID = 0x0001A26Fu;
            std::string cellName = "Whiterun Bannered Mare";
            bool cellIsInterior = true;
            // Whether the engine's record store is up. Absent before data load.
            bool dataHandlerPresent = true;

            // The exterior the player's cell belongs to, and where in it. Only
            // a cell with all three can be looked up in a precomputed grid.
            RE::TESWorldSpace* playerCellWorldSpace = nullptr;
            std::int16_t playerCellX = 0;
            std::int16_t playerCellY = 0;

            // Editor IDs, which the engine only retains at runtime with
            // powerofthree's Tweaks installed. Empty is the no-Tweaks case, and
            // several predicates degrade open on it by design.
            std::string cellEditorID = "WhiterunBanneredMare";
            std::string locationEditorID = "WhiterunLocation";

            // The player location's parentLoc, if any. Vanilla nests these —
            // SovngardeHallofHeroesLocation sits under SovngardeLocation — and
            // several predicates walk the chain rather than reading only the
            // leaf. Empty means the leaf has no parent.
            std::string locationParentEditorID;

            // Stand the player in a location the test built with AddLocation,
            // rather than in the one fabricated from the fields above. The
            // fabricated location has no keywords and no parent worth walking,
            // so any module that reads either needs this.
            RE::BGSLocation* playerLocationOverride = nullptr;

            // The scripted scene the player is standing in, if any.
            bool playerInScene = false;
            bool sceneIsPlaying = true;

            // Per-actor state the snapshot reads beyond the liveness flags.
            std::string actorDisplayName = "Ysolda";
            bool actorIsPlayerTeammate = false;

            // How far the player has levelled. What an encounter is scaled
            // against, and the only thing anything here asks an actor's level
            // for.
            std::uint16_t actorLevel = 1;
            bool actorIsBleedingOut = false;
        } world;

        struct SkyState
        {
            bool present = true;
            // RE::Sky::Mode as a raw value, so this header stays free of
            // engine enums: kNone 0, kInterior 1, kSkyDomeOnly 2, kFull 3.
            std::uint32_t mode = 3; // kFull
            bool hasWeather = true;
            std::uint32_t weatherFormID = 0x00010E1Cu;
            std::uint8_t weatherFlags = 0x02;
            std::uint8_t windSpeed = 40;
            std::int8_t thunderLightningFrequency = 7;
        } sky;

        // Vanilla keywords and the locations that carry them.
        //
        // Keywords live in a pool that OUTLIVES this EngineMock, because
        // LocationKeywords resolves its whole table once per process behind a
        // function-local static and never re-resolves. That cache is correct
        // for the game — keyword forms are static vanilla data — but it means
        // a pointer handed out under one mock is still held under the next, so
        // the objects must not be destroyed with it.
        RE::BGSKeyword* AddKeyword(std::string_view editorID);

        // A location carrying the named keywords, registered in the form table.
        // Fabricate a BGSLocation. `editorID` is optional because most callers
        // only need the display name, but a module that looks a location up by
        // editor ID — or falls back to one when the display name is empty —
        // needs it registered in both the editor-ID table and the per-form map.
        RE::BGSLocation* AddLocation(std::uint32_t formID,
                                     std::string name,
                                     std::vector<std::string> keywordEditorIDs,
                                     std::string editorID = {});

        // Point `child` at `parent` for the parentLoc walk.
        void SetLocationParent(RE::BGSLocation* child, RE::BGSLocation* parent);

        // File a unique NPC as living at a location, the way the LCUN
        // subrecord does. `editorLocation` is the finer-grained place inside
        // it — a town's row for a family names their house — and is optional.
        void AddResident(RE::BGSLocation* location, RE::TESNPC* npc, RE::BGSLocation* editorLocation = nullptr);

        // Put an NPC in a faction at a rank, as their base record does.
        // Distinct from SetFactionRank, which answers the runtime question an
        // Actor is asked; this is the authored membership a sweep of the
        // records reads.
        void JoinFaction(RE::TESNPC* npc, RE::TESFaction* faction, std::int8_t rank = 0);

        // A race, carrying whatever keywords a test names. The keyword on a
        // race is how anything tells a person from a wolf, so a fabricated NPC
        // is given one that carries ActorTypeNPC unless a test says otherwise.
        RE::TESRace* AddRace(std::uint32_t formID, std::string editorID, std::vector<std::string> keywordEditorIDs);

        // The race every fabricated NPC gets. Made on first use and shared,
        // which is how a race works in the game too.
        RE::TESRace* DefaultPeopleRace();
        void SetNPCRace(RE::TESNPC* npc, RE::TESRace* race);

        // Give a form an editor ID after the fact. Records left in a file for
        // testing are recognised by theirs, which is the only thing marking
        // them out from the real ones.
        void SetEditorIDOf(RE::TESForm* form, std::string editorID);

        // Where a reference was placed in the editor, which is not where it is
        // standing now. Only the placement is authored data.
        void SetEditorLocation(RE::TESObjectREFR* ref, RE::BGSLocation* location);

        Runtime runtime() const
        {
            return runtime_;
        }

        // The installed instance, or nullptr when none is alive. Used by the
        // mocked engine functions in EngineMock.cpp.
        static EngineMock* Current();

    private:
        Runtime runtime_;
    };
} // namespace NarrativeEngine::Testing
