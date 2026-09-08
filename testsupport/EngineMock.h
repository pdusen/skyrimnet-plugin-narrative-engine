#pragma once

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
    class Sky;
    class TESFaction;
    class TESObjectCELL;
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
        RE::TESForm* AddBook(std::uint32_t formID);

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

        // A stand-in faction to hold ranks against.
        RE::TESFaction* AddFaction(std::uint32_t formID);

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
