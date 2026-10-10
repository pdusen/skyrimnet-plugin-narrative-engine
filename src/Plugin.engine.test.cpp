#include <Plugin.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>

#include <DecisionLog.h>
#include <PhaseTracker.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

// Tests for the plugin's own front door.
//
// Startup is where the mod meets SKSE, and it is four handshakes in a row:
// initialise the library, take the message channel, claim a co-save id, and
// hand over the three serialization callbacks. Any one of them failing quietly
// produces a mod that loads, logs nothing unusual, and then does nothing at
// all -- so what these cases pin is that each is asked for and that a refusal
// is reported rather than carried on from.
//
// Past the handshakes, everything the plugin does lives inside those
// callbacks: the co-save dispatch that routes each record to whichever module
// owns it, and the revert that puts every module back at once. Neither is
// reachable except through the registration, so the harness keeps the
// callbacks Startup hands over (see EngineMock's SKSEInterfaceState) and the
// cases call them the way SKSE would.
//
// The message handler is deliberately not driven here. Its kDataLoaded arm
// initialises every subsystem in the mod against a live load order, which is
// a world each of those subsystems already builds for itself in its own tests;
// what is left for this file is the wiring.

namespace
{
    namespace DecisionLog = NarrativeEngine::DecisionLog;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr const char* kSettings = "[General]\nbDebugMode=0\n";

    // The interface SKSE hands a plugin on the way up. Never read by anything
    // under test -- Startup passes it straight to SKSE::Init -- so a valid
    // address is all it has to be.
    const SKSE::LoadInterface* FakeLoadInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<const SKSE::LoadInterface*>(storage);
    }

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    bool OpenedRecord(const EngineMock& engine, std::uint32_t type)
    {
        return std::any_of(engine.cosave.opened.begin(), engine.cosave.opened.end(), [&](const auto& record) {
            return record.type == type;
        });
    }
} // namespace

TEST_CASE("Plugin wires itself into SKSE", "[Plugin][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};

    SECTION("when SKSE hands over everything it should")
    {
        REQUIRE(NarrativeEngine::Startup(FakeLoadInterface()));

        SECTION("should initialise the library before asking it for anything")
        {
            REQUIRE(engine.skse.initCalls == 1);
        }

        SECTION("should take the message channel")
        {
            // Every subsystem in the mod is brought up from a message on this
            // channel. Without it the plugin loads and nothing else happens.
            REQUIRE(engine.skse.messageListener != nullptr);
        }

        SECTION("should claim the co-save id its records live under")
        {
            // Frozen: change it and every existing save's co-save data is
            // orphaned, silently, with the mod reading as freshly installed.
            REQUIRE(engine.skse.uniqueID != 0);
        }

        SECTION("should hand over all three save callbacks")
        {
            // Save without load is data written and never read; load without
            // revert restores the previous game's state on top of the new one.
            REQUIRE(engine.skse.saveCallback != nullptr);
            REQUIRE(engine.skse.loadCallback != nullptr);
            REQUIRE(engine.skse.revertCallback != nullptr);
        }
    }

    SECTION("when the message channel is unavailable")
    {
        engine.skse.messagingPresent = false;

        SECTION("should refuse to load")
        {
            // Reported rather than carried on from: a plugin that returns
            // success here loads into the game and then never wakes up.
            REQUIRE_FALSE(NarrativeEngine::Startup(FakeLoadInterface()));
        }
    }

    SECTION("when the channel refuses the listener")
    {
        engine.skse.listenerRegisters = false;

        SECTION("should refuse to load")
        {
            REQUIRE_FALSE(NarrativeEngine::Startup(FakeLoadInterface()));
        }
    }

    SECTION("when the serialization interface is unavailable")
    {
        engine.skse.serializationPresent = false;

        SECTION("should refuse to load")
        {
            // Without it nothing the mod knows survives a save, and every
            // load starts a new story over the top of the old one.
            REQUIRE_FALSE(NarrativeEngine::Startup(FakeLoadInterface()));
        }
    }
}

TEST_CASE("Plugin writes every subsystem into the co-save", "[Plugin][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    REQUIRE(NarrativeEngine::Startup(FakeLoadInterface()));
    REQUIRE(engine.skse.saveCallback != nullptr);

    SECTION("when the game is saved")
    {
        engine.skse.saveCallback(FakeInterface());

        SECTION("should give the story's own state a record")
        {
            REQUIRE(OpenedRecord(engine, PhaseTracker::kRecordTypeId));
            REQUIRE(OpenedRecord(engine, DecisionLog::kRecordTypeId));
        }

        SECTION("should open one record per subsystem rather than share")
        {
            // Each record is versioned independently, and one shared record
            // would make every subsystem's format change everybody else's.
            std::vector<std::uint32_t> types;
            for (const auto& record : engine.cosave.opened) {
                types.push_back(record.type);
            }
            const auto before = types.size();
            std::sort(types.begin(), types.end());
            types.erase(std::unique(types.begin(), types.end()), types.end());
            REQUIRE(types.size() == before);
            REQUIRE(types.size() > 8);
        }
    }
}

TEST_CASE("Plugin routes a loaded record to whoever owns it", "[Plugin][engine]")
{
    // The dispatch every module's own OnLoad hangs off. A record routed to
    // the wrong module reads the wrong bytes out of a shared stream and
    // desynchronises everything that follows it.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    REQUIRE(NarrativeEngine::Startup(FakeLoadInterface()));
    REQUIRE(engine.skse.loadCallback != nullptr);
    PhaseTracker::Reset();
    DecisionLog::Clear();

    SECTION("when the save carries a record nothing in this build owns")
    {
        // A co-save from a newer build, or from one with a subsystem this
        // build has dropped. The walk must step over it and keep going --
        // stopping there would silently discard everything written after it.
        engine.skse.records = {
            {0xDEADBEEFu, 1, 4},
            {PhaseTracker::kRecordTypeId, 1, 0},
        };

        SECTION("should step over it and carry on")
        {
            engine.skse.loadCallback(FakeInterface());
            REQUIRE(engine.skse.recordCursor == 2);
        }
    }

    SECTION("when the save is empty")
    {
        SECTION("should read nothing and finish")
        {
            // A save made before the mod was installed. Every module keeps
            // the baseline the revert just gave it.
            engine.skse.loadCallback(FakeInterface());
            REQUIRE(DecisionLog::Tail(1).empty());
        }
    }

    SECTION("when there is no interface at all")
    {
        SECTION("should do nothing")
        {
            engine.skse.records = {{PhaseTracker::kRecordTypeId, 1, 0}};
            engine.skse.loadCallback(nullptr);
            REQUIRE(engine.skse.recordCursor == 0);
        }
    }
}

TEST_CASE("Plugin puts every subsystem back on a revert", "[Plugin][engine]")
{
    // Revert runs before a load and on a new game, and it is the only thing
    // standing between the previous playthrough's state and this one.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    REQUIRE(NarrativeEngine::Startup(FakeLoadInterface()));
    REQUIRE(engine.skse.revertCallback != nullptr);

    DecisionLog::DecisionRecord record;
    record.realTimeSec = 99.0;
    record.tensionScore = 40;
    record.narrativeNote = "from the game before this one";
    DecisionLog::Append(record);
    REQUIRE_FALSE(DecisionLog::Tail(1).empty());

    SECTION("when a revert comes in")
    {
        engine.skse.revertCallback(FakeInterface());

        SECTION("should clear what the last game left behind")
        {
            REQUIRE(DecisionLog::Tail(1).empty());
        }

        SECTION("should put the story back at its beginning")
        {
            REQUIRE(PhaseTracker::Get() == PhaseTracker::Phase::Exposition);
        }
    }
}
