#include <CombatEventLog.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <PhaseTracker.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <SKSE/Interfaces.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

// Tests for the combat event source.
//
// Five kinds of event, of which the poll produces three: the player entering
// and leaving combat, and an actor getting back on their feet after bleeding
// out. The other two come from engine sinks, which fire on threads no test can
// reach — those are noted where they bite rather than pretended at.
//
// The edge detection is the whole of it. Combat state is polled, not pushed, so
// the module compares against what it last saw; getting that wrong gives a
// "combat has begun" event on every single tick of a fight. And the baseline is
// seeded at load rather than left false, because a save made mid-battle would
// otherwise report the fight starting again the moment it was loaded.
//
// Bleedout recovery is deliberately narrow. Only an actor the player can still
// see counts: one who got up out of range, died, or unloaded is dropped
// silently, because none of those is something the player witnessed and the
// tail is what the Director reasons from.

namespace
{
    namespace CombatEventLog = NarrativeEngine::CombatEventLog;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    constexpr std::uint32_t kBandit = 0x0001A6A0u;

    // A radius wide enough that "in range" is the default, so a case that wants
    // out-of-range has to say so by moving the actor.
    // The history feed is switched on here too, because the queue that feeds
    // the session archive is gated on it and is otherwise never filled.
    constexpr const char* kSettings = "[CombatEvents]\niHitRadiusUnits=6000\niMaxStored=128\n"
                                      "[EventHistory]\nbEventHistoryEnabled=1\n";

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    void Poll()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([](const PluginThread::Token& pt) { CombatEventLog::Poll(pt); });
    }

    nlohmann::json Tail()
    {
        return CombatEventLog::GetRenderedTail(0.0);
    }

    std::size_t TailSize()
    {
        return Tail().size();
    }

    // How many events of a given kind the tail holds.
    std::size_t CountKind(std::string_view kind)
    {
        const auto tail = Tail();
        return static_cast<std::size_t>(std::count_if(
            tail.begin(), tail.end(), [&](const nlohmann::json& e) { return e.value("ne_kind", "") == kind; }));
    }

    // The module's state is process-wide, so each case starts clean.
    struct FreshLog
    {
        FreshLog()
        {
            CombatEventLog::OnRevert();
        }

        FreshLog(const FreshLog&) = delete;
        FreshLog& operator=(const FreshLog&) = delete;
    };
} // namespace

TEST_CASE("CombatEventLog watches the player enter and leave combat", "[CombatEventLog][engine]")
{
    // Happy path, re-run per leaf: a player out of combat, with the log's
    // baseline already agreeing.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    engine.player.inCombat = false;
    Poll();

    SECTION("when the player is drawn into a fight")
    {
        engine.player.inCombat = true;
        Poll();

        SECTION("should emit that combat began")
        {
            REQUIRE(CountKind("combat_start") == 1);
        }

        SECTION("should not emit again while the fight goes on")
        {
            // Polled rather than pushed, so without the comparison against the
            // last observed state this would fire on every tick of a battle and
            // drown the tail in one event.
            Poll();
            Poll();
            REQUIRE(CountKind("combat_start") == 1);
        }
    }

    SECTION("when the fight ends")
    {
        engine.player.inCombat = true;
        Poll();
        engine.player.inCombat = false;
        Poll();

        SECTION("should emit that combat ended")
        {
            REQUIRE(CountKind("combat_end") == 1);
        }

        SECTION("should keep both halves of the encounter")
        {
            // The pair is what makes a fight legible to the Director: a start
            // with no end reads as a battle still raging.
            REQUIRE(CountKind("combat_start") == 1);
            REQUIRE(TailSize() == 2);
        }
    }

    SECTION("when the player never fights")
    {
        SECTION("should emit nothing")
        {
            Poll();
            Poll();
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when the player singleton is unavailable")
    {
        engine.player.present = false;

        SECTION("should emit nothing rather than crash")
        {
            // Pre-load and mid-teardown. The poll runs from the tick either
            // way.
            Poll();
            REQUIRE(TailSize() == 0);
        }
    }
}

TEST_CASE("CombatEventLog names the player in its events", "[CombatEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    engine.player.inCombat = false;
    Poll();

    SECTION("when combat begins")
    {
        engine.world.actorDisplayName = "Dovahkiin";
        engine.player.inCombat = true;
        Poll();

        SECTION("should render text a model can read")
        {
            // The tail is merged into the same recent-events array SkyrimNet
            // fills, so the sentence is the payload; the kind is only for the
            // merge step.
            const auto tail = Tail();
            REQUIRE(tail.size() == 1);
            REQUIRE(tail[0].value("type", "") == "combat_event");
            REQUIRE_FALSE(tail[0].value("text", "").empty());
        }

        SECTION("should mark the actor as a named one")
        {
            // Unnamed actors are rendered generically; the player never is.
            REQUIRE(Tail()[0].value("ne_actor_is_named", false));
        }
    }
}

TEST_CASE("CombatEventLog reports only recoveries the player saw", "[CombatEventLog][engine]")
{
    // An actor who went down while the player watched, tracked so the module
    // can notice them getting up again. Registered as a loaded actor because
    // that is the list the module seeds its watch set from.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* bandit = engine.AddLoadedActor(kBandit);
    REQUIRE(bandit != nullptr);
    bandit->data.location = RE::NiPoint3{0.0f, 0.0f, 0.0f};
    EngineMock::SetActorBleedingOut(bandit, true);
    CombatEventLog::OnPostLoadGame();

    SECTION("when a downed actor gets back up in view")
    {
        EngineMock::SetActorBleedingOut(bandit, false);
        Poll();

        SECTION("should report it")
        {
            REQUIRE(CountKind("regain_footing") == 1);
        }

        SECTION("should stop watching them")
        {
            // The watch set is what the poll walks every tick, so an entry that
            // is never removed is a per-actor cost that only grows.
            Poll();
            REQUIRE(CountKind("regain_footing") == 1);
        }
    }

    SECTION("when the actor gets up out of the player's sight")
    {
        bandit->data.location = RE::NiPoint3{100000.0f, 0.0f, 0.0f};
        EngineMock::SetActorBleedingOut(bandit, false);
        Poll();

        SECTION("should drop it silently")
        {
            // Nothing the player could witness is nothing the Director should
            // reason about. Reporting it would put events in the tail that
            // contradict what the player saw.
            REQUIRE(CountKind("regain_footing") == 0);
        }
    }

    SECTION("when the actor dies instead")
    {
        engine.forms.actorIsDead = true;
        EngineMock::SetActorBleedingOut(bandit, false);
        Poll();

        SECTION("should say nothing about it")
        {
            // A death is already in SkyrimNet's own event log; emitting a
            // recovery here would contradict it.
            REQUIRE(CountKind("regain_footing") == 0);
        }
    }

    SECTION("when the actor is still down")
    {
        Poll();

        SECTION("should keep waiting")
        {
            REQUIRE(CountKind("regain_footing") == 0);
        }

        SECTION("should still notice a later recovery")
        {
            EngineMock::SetActorBleedingOut(bandit, false);
            Poll();
            REQUIRE(CountKind("regain_footing") == 1);
        }
    }
}

TEST_CASE("CombatEventLog seeds its baseline at load", "[CombatEventLog][engine]")
{
    // A save made mid-battle. Without seeding, the first poll after the load
    // sees a combat flag that "changed" from its default false and reports the
    // fight starting again — an event the player already lived through.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    engine.player.inCombat = true;

    SECTION("when the save was made in combat")
    {
        CombatEventLog::OnPostLoadGame();
        Poll();

        SECTION("should not report the fight starting again")
        {
            REQUIRE(CountKind("combat_start") == 0);
        }

        SECTION("should still report it ending")
        {
            engine.player.inCombat = false;
            Poll();
            REQUIRE(CountKind("combat_end") == 1);
        }
    }
}

TEST_CASE("CombatEventLog bounds what it keeps", "[CombatEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[CombatEvents]\niHitRadiusUnits=6000\niMaxStored=3\n"};
    const FreshLog fresh;
    engine.player.inCombat = false;
    Poll();

    SECTION("when more events happen than the cap allows")
    {
        for (int i = 0; i < 6; ++i) {
            engine.player.inCombat = !engine.player.inCombat;
            Poll();
        }

        SECTION("should keep no more than the cap")
        {
            // The tail is rendered into every Director prompt, so the bound is
            // about prompt cost as much as memory.
            REQUIRE(TailSize() == 3);
        }
    }
}

TEST_CASE("CombatEventLog prunes to the current encounter", "[CombatEventLog][engine]")
{
    // Not to the phase boundary, which is what the other event logs do. A fight
    // that is still going has to keep its opening event across a phase change,
    // or the Director reads a battle in progress as having no beginning.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    PhaseTracker::Reset();
    engine.player.inCombat = false;
    Poll();

    SECTION("when a fight is still going")
    {
        engine.player.inCombat = true;
        Poll();
        REQUIRE(TailSize() == 1);

        SECTION("should keep the event that started it")
        {
            PhaseTracker::AdvanceTo(PhaseTracker::Phase::RisingAction);
            REQUIRE(CountKind("combat_start") == 1);
        }
    }

    SECTION("when no fight is in progress")
    {
        engine.player.inCombat = true;
        Poll();
        engine.player.inCombat = false;
        Poll();
        REQUIRE(TailSize() == 2);

        SECTION("should drop the whole of the previous phase")
        {
            // With no encounter to anchor to there is nothing worth carrying:
            // every event belongs to a phase the Director has already answered.
            PhaseTracker::AdvanceTo(PhaseTracker::Phase::RisingAction);
            REQUIRE(TailSize() == 0);
        }
    }
}

TEST_CASE("CombatEventLog survives a save and load", "[CombatEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    // A phase has to have been entered first: the save path prunes to the
    // current phase, and with no phase ever entered the cutoff is zero, which
    // the module reads as "no baseline" and clears the log rather than writing
    // events it cannot place.
    PhaseTracker::Reset();
    engine.player.inCombat = false;
    Poll();
    engine.player.inCombat = true;
    Poll();
    REQUIRE(TailSize() == 1);

    SECTION("when the log is written and read back")
    {
        CombatEventLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        CombatEventLog::OnRevert();
        REQUIRE(TailSize() == 0);
        CombatEventLog::OnLoad(FakeInterface(), 1, 0);

        SECTION("should restore the events")
        {
            REQUIRE(TailSize() == 1);
        }

        SECTION("should restore what kind of event it was")
        {
            REQUIRE(CountKind("combat_start") == 1);
        }
    }

    SECTION("when the record stamps its type")
    {
        CombatEventLog::OnSave(FakeInterface());

        SECTION("should use the frozen record type")
        {
            REQUIRE(engine.cosave.opened.size() == 1);
            REQUIRE(engine.cosave.opened[0].type == CombatEventLog::kRecordTypeId);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            CombatEventLog::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        CombatEventLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        CombatEventLog::OnRevert();

        SECTION("should restore nothing")
        {
            CombatEventLog::OnLoad(FakeInterface(), 99, 0);
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing")
        {
            CombatEventLog::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
            CombatEventLog::OnLoad(nullptr, 1, 0);
            REQUIRE(TailSize() == 1);
        }
    }
}

TEST_CASE("CombatEventLog feeds the history writer", "[CombatEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    engine.player.inCombat = false;
    Poll();
    engine.player.inCombat = true;
    Poll();

    SECTION("when an event is emitted")
    {
        const auto drained = CombatEventLog::DrainHistoryTail();

        SECTION("should hand over a rendered line")
        {
            REQUIRE(drained.size() == 1);
            REQUIRE(drained[0].sourceKind.starts_with("internal/combat_event/"));
            REQUIRE_FALSE(drained[0].body.empty());
        }

        SECTION("should hand each entry over only once")
        {
            // The archive appends what it drains, so a drain that copied
            // instead of consuming would repeat every event on every flush.
            REQUIRE(CombatEventLog::DrainHistoryTail().empty());
        }
    }
}
