#include <TravelEventLog.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <EventLogUtil.h>
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
#include <string_view>
#include <vector>

// Tests for the travel event source.
//
// The player's geography is the context every other subsystem is judged
// against — where they are, which hold they are in, whether they just arrived.
// The module derives all of it from one polled snapshot and emits on the
// differences, and several of those differences are subtle enough to be worth
// pinning individually.
//
// Interior-to-interior transitions are suppressed. Walking between two rooms of
// a dungeon, or through a loading door, changes the location every time and
// means nothing; without the gate a dungeon crawl produces an event per zone
// and drowns everything else out.
//
// The last known hold is carried across an interior visit. Interiors often
// resolve to no hold at all, so a naive diff would report leaving Whiterun Hold
// on entering the Bannered Mare and re-entering it on the way out — twice per
// building, all day.
//
// The module has two readers and they see different things, which is what
// splits this file in half. GetRenderedTail is what the Director is shown: a
// condensation pass groups events that landed within a settings-defined window
// of each other and replaces the group with a single summary, or with nothing
// at all when the group ends where it began. DrainHistoryTail is the archive's
// feed and is uncondensed — one entry per emitted event, tagged with the kind.
//
// Every event a poll-driven test emits lands microseconds after the last, and
// the grouping test is a comparison of two Unix timestamps held as doubles,
// which at present-day magnitudes cannot resolve intervals below about half a
// microsecond. Two events in one poll therefore group or not depending on where
// the clock happened to land. So the emission cases below read the history
// feed, where each event is its own entry no matter what the clock did, and the
// condensation cases open the window wide enough that the whole journey is one
// run either way. Neither kind of case is left depending on that coin flip.

namespace
{
    namespace TravelEventLog = NarrativeEngine::TravelEventLog;
    namespace EventLogUtil = NarrativeEngine::EventLogUtil;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    // The history feed is the emission cases' window onto the module, and it is
    // gated on the writer's master switch, so every one of them turns it on.
    constexpr const char* kSettings = "[EventHistory]\nbEventHistoryEnabled=1\n";

    // A window wide enough that everything a case emits lands in one run —
    // which is what the shipping default does to a journey taken at speed.
    constexpr const char* kCondensingSettings = "[TravelEvents]\niTravelCondensationWindowSeconds=300\n";

    constexpr std::uint32_t kWhiterunHold = 0x00016BE4u;
    constexpr std::uint32_t kFalkreathHold = 0x0001680Fu;
    constexpr std::uint32_t kWhiterunCity = 0x00018A56u;
    constexpr std::uint32_t kRiverwood = 0x00018A57u;
    constexpr std::uint32_t kBanneredMare = 0x00018A58u;
    constexpr std::uint32_t kFalkreathTown = 0x00018A59u;
    constexpr std::uint32_t kFalkreathWilds = 0x00018A5Au;

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    void Poll()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [](const PluginThread::Token& pt) { TravelEventLog::Poll(pt, 1.0); });
    }

    // Everything emitted since the last call. Draining consumes, so a case
    // takes this once and asks the result its questions.
    std::vector<EventLogUtil::HistoryEntry> Emitted()
    {
        return TravelEventLog::DrainHistoryTail();
    }

    std::size_t Count(const std::vector<EventLogUtil::HistoryEntry>& emitted, std::string_view kind)
    {
        const std::string want = std::string("internal/travel_event/") + std::string(kind);
        return static_cast<std::size_t>(std::count_if(
            emitted.begin(), emitted.end(), [&](const EventLogUtil::HistoryEntry& e) { return e.sourceKind == want; }));
    }

    bool AnyBodyMentions(const std::vector<EventLogUtil::HistoryEntry>& emitted, std::string_view text)
    {
        return std::any_of(emitted.begin(), emitted.end(), [&](const EventLogUtil::HistoryEntry& e) {
            return e.body.find(text) != std::string::npos;
        });
    }

    nlohmann::json Tail()
    {
        return TravelEventLog::GetRenderedTail(0.0);
    }

    std::size_t TailSize()
    {
        return Tail().size();
    }

    std::string FirstText()
    {
        const auto tail = Tail();
        return tail.empty() ? std::string{} : tail[0].value("text", std::string{});
    }

    // The hold a location belongs to is derived by walking its parent chain to
    // an ancestor carrying LocTypeHold, so a case that wants the player in a
    // hold builds that chain out of registered locations.
    RE::BGSLocation* HoldNamed(EngineMock& engine, std::uint32_t formID, const char* name, const char* editorID)
    {
        (void)engine.AddKeyword("LocTypeHold");
        return engine.AddLocation(formID, name, {"LocTypeHold"}, editorID);
    }

    // Stands the player in a named location belonging to `hold`. A null hold
    // means the location resolves to none, which is what an unclassified
    // scripted interior does in the live game.
    void StandIn(EngineMock& engine,
                 std::uint32_t locationFormID,
                 const char* locationName,
                 RE::BGSLocation* hold,
                 bool interior)
    {
        auto* here = engine.AddLocation(locationFormID, locationName, {});
        engine.SetLocationParent(here, hold);
        engine.world.playerHasLocation = true;
        engine.world.playerLocationOverride = here;
        engine.world.cellIsInterior = interior;
    }

    // Stands the player on unmarked ground: no named location at all.
    void StandInWilderness(EngineMock& engine, bool interior)
    {
        engine.world.playerHasLocation = false;
        engine.world.playerLocationOverride = nullptr;
        engine.world.cellIsInterior = interior;
    }

    struct FreshLog
    {
        FreshLog()
        {
            TravelEventLog::OnRevert();
        }

        FreshLog(const FreshLog&) = delete;
        FreshLog& operator=(const FreshLog&) = delete;
    };
} // namespace

TEST_CASE("TravelEventLog seeds a baseline before emitting", "[TravelEventLog][engine]")
{
    // The happy path, re-run per leaf: the player standing in a named exterior
    // location inside a hold, with the history feed on.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);

    SECTION("when the first poll observes where the player is")
    {
        Poll();

        SECTION("should emit nothing")
        {
            // There is no previous position to have travelled from. Emitting
            // here would report an arrival every time a save was loaded.
            REQUIRE(Emitted().empty());
        }
    }

    SECTION("when nothing has changed since the last poll")
    {
        Poll();
        Poll();

        SECTION("should emit nothing")
        {
            // The poll runs on every unpaused tick, so by far the commonest
            // case is a player who has not gone anywhere.
            REQUIRE(Emitted().empty());
        }
    }
}

TEST_CASE("TravelEventLog reports the location chain", "[TravelEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();

    SECTION("when the player moves to another named place")
    {
        StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
        Poll();
        const auto emitted = Emitted();

        SECTION("should report leaving the old one")
        {
            REQUIRE(Count(emitted, "left_location") == 1);
        }

        SECTION("should report entering the new one")
        {
            // The pair is what makes a journey legible: an arrival with no
            // departure reads as the player having always been there.
            REQUIRE(Count(emitted, "entered_location") == 1);
        }

        SECTION("should say where the departure was from")
        {
            REQUIRE(AnyBodyMentions(emitted, "Riverwood"));
        }
    }

    SECTION("when the player walks out into unmarked ground")
    {
        StandInWilderness(engine, /*interior=*/false);
        Poll();
        const auto emitted = Emitted();

        SECTION("should report leaving the named place")
        {
            REQUIRE(Count(emitted, "left_location") == 1);
        }

        SECTION("should not claim they entered anywhere")
        {
            // Wilderness has no name to enter. An arrival event with an empty
            // name would put a sentence in the tail about nowhere.
            REQUIRE(Count(emitted, "entered_location") == 0);
        }
    }

    SECTION("when the player walks out of unmarked ground into a named place")
    {
        StandInWilderness(engine, /*interior=*/false);
        Poll();
        StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
        Poll();
        const auto emitted = Emitted();

        SECTION("should report the arrival")
        {
            REQUIRE(Count(emitted, "entered_location") == 1);
        }

        SECTION("should not invent a departure from nowhere")
        {
            // Only the one departure, from Riverwood at the start. The step out
            // of wilderness has no origin name to report.
            REQUIRE(Count(emitted, "left_location") == 1);
        }
    }
}

TEST_CASE("TravelEventLog suppresses interior hops", "[TravelEventLog][engine]")
{
    // Walking between two rooms of a dungeon changes the location every time
    // and means nothing. Without the gate a single dungeon produces an event
    // per zone and buries everything else.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kBanneredMare, "The Bannered Mare", whiterun, /*interior=*/true);
    Poll();

    SECTION("when the player moves between two interiors")
    {
        StandIn(engine, kRiverwood, "Warmaidens", whiterun, /*interior=*/true);
        Poll();

        SECTION("should emit nothing")
        {
            REQUIRE(Emitted().empty());
        }
    }

    SECTION("when the player steps outside")
    {
        StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
        Poll();

        SECTION("should emit the transition")
        {
            // Interior to exterior is a real move: leaving a building is
            // something an onlooker would notice.
            REQUIRE(Count(Emitted(), "entered_location") == 1);
        }
    }
}

TEST_CASE("TravelEventLog tracks holds across an interior visit", "[TravelEventLog][engine]")
{
    // Interiors often belong to no hold at all. A naive diff would report
    // leaving Whiterun Hold on entering the inn and re-entering it on the way
    // out — twice per building, all day.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    auto* falkreath = HoldNamed(engine, kFalkreathHold, "Falkreath Hold", "FalkreathHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();

    SECTION("when the player goes indoors and out again")
    {
        // The interior resolves to no hold at all, which is the state the
        // carry-over exists for.
        StandIn(engine, kBanneredMare, "The Sleeping Giant", nullptr, /*interior=*/true);
        Poll();
        StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
        Poll();

        SECTION("should never report a hold crossing")
        {
            REQUIRE(Count(Emitted(), "crossed_holds") == 0);
        }
    }

    SECTION("when the player genuinely crosses into another hold")
    {
        StandIn(engine, kFalkreathTown, "Falkreath", falkreath, /*interior=*/false);
        Poll();
        const auto emitted = Emitted();

        SECTION("should report the crossing")
        {
            // The event the whole hold-tracking apparatus exists for: leaving
            // one jarl's authority for another's is the most consequential
            // thing a journey can do.
            REQUIRE(Count(emitted, "crossed_holds") == 1);
        }

        SECTION("should name both holds")
        {
            REQUIRE(AnyBodyMentions(emitted, "Whiterun Hold"));
            REQUIRE(AnyBodyMentions(emitted, "Falkreath Hold"));
        }
    }

    SECTION("when the player crosses a hold border by stepping indoors")
    {
        StandIn(engine, kFalkreathTown, "Dead Mans Drink", falkreath, /*interior=*/true);
        Poll();

        SECTION("should not report a hold crossing")
        {
            // Hold events are exterior-to-exterior only. A doorway is not a
            // border, and an interior's own hold data is not trustworthy
            // enough to declare one on.
            REQUIRE(Count(Emitted(), "crossed_holds") == 0);
        }
    }
}

TEST_CASE("TravelEventLog reports wilderness", "[TravelEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    auto* falkreath = HoldNamed(engine, kFalkreathHold, "Falkreath Hold", "FalkreathHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();

    SECTION("when the player leaves a named place for open country")
    {
        StandInWilderness(engine, /*interior=*/false);
        Poll();

        SECTION("should say they are in the wilderness")
        {
            // Distinct from simply having left somewhere: the Director treats
            // open country as a place with its own character, and it is where
            // an ambush belongs.
            REQUIRE(Count(Emitted(), "entered_wilderness") == 1);
        }
    }

    SECTION("when the player leaves a named place and changes hold at once")
    {
        // A nameless exterior location that still belongs to another hold —
        // the branch the wilderness check is guarded by. With the hold
        // changing, left_location and crossed_holds already describe the move,
        // and a wilderness event would be a third account of the same step.
        auto* falkreathWilds = engine.AddLocation(kFalkreathWilds, "", {});
        engine.SetLocationParent(falkreathWilds, falkreath);
        engine.world.playerHasLocation = true;
        engine.world.playerLocationOverride = falkreathWilds;
        engine.world.cellIsInterior = false;
        Poll();
        const auto emitted = Emitted();

        SECTION("should not also announce the wilderness")
        {
            REQUIRE(Count(emitted, "entered_wilderness") == 0);
        }

        SECTION("should announce the hold crossing")
        {
            REQUIRE(Count(emitted, "crossed_holds") == 1);
        }
    }
}

TEST_CASE("TravelEventLog marks a fast-travel arrival", "[TravelEventLog][engine]")
{
    // A fast-travel arrival replaces the entered/crossed pair rather than
    // adding to it: the one event already carries both endpoints, and the
    // others would describe the same journey a second time.
    //
    // The flag is raised by a TESFastTravelEndEvent sink that fires on an
    // engine thread and cannot be reached from a test, so what is checked here
    // is the other half — that an ordinary walked transition does not claim to
    // be fast travel, and still emits everything the arrival would replace.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    auto* falkreath = HoldNamed(engine, kFalkreathHold, "Falkreath Hold", "FalkreathHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();

    SECTION("when the player walks somewhere")
    {
        StandIn(engine, kFalkreathTown, "Falkreath", falkreath, /*interior=*/false);
        Poll();
        const auto emitted = Emitted();

        SECTION("should not call it a fast travel")
        {
            REQUIRE(Count(emitted, "fast_travel_arrived") == 0);
        }

        SECTION("should report the walk in full")
        {
            REQUIRE(Count(emitted, "left_location") == 1);
            REQUIRE(Count(emitted, "entered_location") == 1);
            REQUIRE(Count(emitted, "crossed_holds") == 1);
        }
    }
}

TEST_CASE("TravelEventLog condenses a run of events", "[TravelEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kCondensingSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();

    SECTION("when the journey ends somewhere new")
    {
        StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
        Poll();

        SECTION("should replace the run with one summary")
        {
            REQUIRE(TailSize() == 1);
            REQUIRE(Tail()[0].value("ne_kind", std::string{}) == "travel_summary");
        }

        SECTION("should name both ends of the journey")
        {
            // The summary is all the Director sees of the run, so it has to
            // carry what the individual events would have said.
            const std::string text = FirstText();
            REQUIRE(text.find("Riverwood") != std::string::npos);
            REQUIRE(text.find("Whiterun") != std::string::npos);
        }
    }

    SECTION("when the player wanders outdoors and comes back")
    {
        StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
        Poll();
        StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
        Poll();

        SECTION("should drop the run entirely")
        {
            // Four events that net out to standing where they started. Nothing
            // happened, and reporting it would spend the Director's attention
            // on a player pacing between two map markers.
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when the round trip went inside somewhere")
    {
        StandIn(engine, kBanneredMare, "The Bannered Mare", whiterun, /*interior=*/true);
        Poll();
        StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
        Poll();

        SECTION("should keep it as a visit")
        {
            // This nets to zero too, but going into a building is the point of
            // the trip — dropping it would erase every errand the player ran.
            REQUIRE(TailSize() == 1);
            REQUIRE(FirstText().find("visited The Bannered Mare") != std::string::npos);
        }
    }
}

TEST_CASE("TravelEventLog bounds what it stores", "[TravelEventLog][engine]")
{
    // The buffer is what a long session accumulates into and what the co-save
    // carries, so it has a ceiling rather than growing until the save does.
    EngineMock engine;
    const ConfiguredSettings settings{"[TravelEvents]\niTravelCondensationWindowSeconds=300\n"
                                      "iTravelEventsMaxStored=2\n"};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();

    SECTION("when more events arrive than the cap allows")
    {
        StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
        Poll();
        StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
        Poll();

        SECTION("should keep the newest and drop the rest")
        {
            // Four events were emitted, and all four together are a round trip
            // the condensation pass discards as noise. Only the return leg
            // survives the cap, so a summary appearing at all — and describing
            // the way back — is the eviction happening from the right end.
            REQUIRE(TailSize() == 1);
            const std::string text = FirstText();
            REQUIRE(text.find("from Whiterun") != std::string::npos);
            REQUIRE(text.find("to Riverwood") != std::string::npos);
        }
    }
}

TEST_CASE("TravelEventLog forgets the previous phase", "[TravelEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    PhaseTracker::Reset();
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();
    StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
    Poll();
    REQUIRE(TailSize() > 0);

    SECTION("when the phase advances")
    {
        SECTION("should drop what the previous phase already saw")
        {
            // Each phase's evaluation reasons about what happened during it.
            // Carrying the last phase's journey forward would have the Director
            // respond twice to one trip.
            PhaseTracker::AdvanceTo(PhaseTracker::Phase::RisingAction);
            REQUIRE(TailSize() == 0);
        }
    }
}

TEST_CASE("TravelEventLog survives a save and load", "[TravelEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    // A phase has to have been entered first: the save path prunes to the
    // current phase anchor, and a zero anchor is read as "no baseline" and
    // clears the log rather than writing events it cannot place.
    PhaseTracker::Reset();
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();
    StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
    Poll();
    const auto before = TailSize();
    REQUIRE(before > 0);

    SECTION("when the log is written and read back")
    {
        TravelEventLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        TravelEventLog::OnRevert();
        REQUIRE(TailSize() == 0);
        TravelEventLog::OnLoad(FakeInterface(), 1, 0);

        SECTION("should restore the events")
        {
            REQUIRE(TailSize() == before);
        }

        SECTION("should restore the place names")
        {
            // The names are what each sentence is rendered from, so a loader
            // that lost them would restore an event about nowhere.
            REQUIRE(FirstText().find("Riverwood") != std::string::npos);
        }
    }

    SECTION("when the record is written")
    {
        TravelEventLog::OnSave(FakeInterface());

        SECTION("should use the frozen record type")
        {
            // Changing it orphans every previously-saved payload.
            REQUIRE(engine.cosave.opened.size() == 1);
            REQUIRE(engine.cosave.opened[0].type == TravelEventLog::kRecordTypeId);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            TravelEventLog::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        TravelEventLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        TravelEventLog::OnRevert();

        SECTION("should restore nothing")
        {
            TravelEventLog::OnLoad(FakeInterface(), 99, 0);
            REQUIRE(TailSize() == 0);
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing")
        {
            TravelEventLog::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
            TravelEventLog::OnLoad(nullptr, 1, 0);
            REQUIRE(TailSize() == before);
        }
    }
}

TEST_CASE("TravelEventLog feeds the history writer", "[TravelEventLog][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();
    StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
    Poll();

    SECTION("when a journey is recorded")
    {
        const auto drained = TravelEventLog::DrainHistoryTail();

        SECTION("should hand over one entry per event")
        {
            // The archive keeps what happened, not the summary the Director is
            // shown, so the condensation pass must not reach it.
            REQUIRE(drained.size() == 2);
            REQUIRE_FALSE(drained[0].body.empty());
        }

        SECTION("should tag each entry with its kind")
        {
            REQUIRE(drained[0].sourceKind == "internal/travel_event/left_location");
            REQUIRE(drained[1].sourceKind == "internal/travel_event/entered_location");
        }

        SECTION("should hand each entry over only once")
        {
            // The writer appends what it drains, so a drain that copied rather
            // than consumed would repeat every journey on every flush.
            REQUIRE(TravelEventLog::DrainHistoryTail().empty());
        }
    }
}

TEST_CASE("TravelEventLog keeps the history queue off when the writer is", "[TravelEventLog][engine]")
{
    // The queue has no bound of its own. Filling it on a session where nothing
    // ever drains it is an unbounded leak, so the master switch gates the push
    // and not just the flush.
    EngineMock engine;
    const ConfiguredSettings settings{"[EventHistory]\nbEventHistoryEnabled=0\n"};
    const FreshLog fresh;
    auto* whiterun = HoldNamed(engine, kWhiterunHold, "Whiterun Hold", "WhiterunHoldLocation");
    StandIn(engine, kRiverwood, "Riverwood", whiterun, /*interior=*/false);
    Poll();

    SECTION("when a journey is recorded with history disabled")
    {
        StandIn(engine, kWhiterunCity, "Whiterun", whiterun, /*interior=*/false);
        Poll();
        REQUIRE(TailSize() > 0);

        SECTION("should queue nothing for the writer")
        {
            REQUIRE(TravelEventLog::DrainHistoryTail().empty());
        }
    }
}
