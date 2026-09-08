#include <VisitState.h>

#include <EngineMock.h>
#include <NPCVisitBeat.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Tests for the visit beat's persisted state.
//
// Two things live here and fail differently.
//
// The snapshot is what a visit needs to put the world back: which NPC was
// taken, where they were standing, and which cell to return them to. Losing it
// mid-visit strands an NPC wherever the beat moved them, permanently, and the
// player's only clue is that a merchant never went home. Its payload has three
// record versions sharing one loader, and the versions differ by fields in the
// middle of the record, not the end — so a loader that misreads one reads
// everything after it out of the wrong offsets.
//
// DerivePhase is the other half: the dashboard's account of what the beat is
// doing, computed from the quest's live stage rather than stored. Stage numbers
// are a contract with hand-authored Papyrus, and two of them — the rollback and
// shutdown stages — deliberately read as Idle so a teardown in progress does
// not display as an active visit.
//
// The quest is resolved once per process behind a call_once, so a case cannot
// change whether it resolves. That is called out where it bites.

namespace
{
    namespace VisitState = NarrativeEngine::VisitState;
    namespace VisitQuery = NarrativeEngine::NPCVisitBeat_Query;
    using NarrativeEngine::Testing::EngineMock;
    using VisitState::Mode;
    using VisitState::Outcome;
    using VisitState::Snapshot;

    // What the beat reports its Discuss substate to be. Stood in for here
    // because linking the beat would pull most of the plugin in behind it.
    VisitQuery::DiscussSubPhase g_subPhase = VisitQuery::DiscussSubPhase::Discussing;

    constexpr std::uint32_t kSenderFormID = 0x0001A6A0u;
    constexpr std::uint32_t kCellFormID = 0x0001A26Fu;
    constexpr std::uint32_t kAnchorFormID = 0x0001A270u;

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    // Every field set to something distinctive, so a writer or reader that
    // transposes two produces a visible mismatch rather than two zeroes that
    // happen to agree.
    Snapshot PopulatedSnapshot()
    {
        Snapshot s;
        s.senderFormID = kSenderFormID;
        s.returnCellFormID = kCellFormID;
        s.returnPosition = RE::NiPoint3{100.5f, -200.25f, 300.125f};
        s.returnAngleZ = 1.75f;
        s.returnAnchorFormID = kAnchorFormID;
        s.briefingText = "Ysolda wants to talk about the mammoth tusk.";
        s.narrationText = "She approaches, wringing her hands.";
        s.topicTag = "debt";
        s.mood = "anxious";
        s.dispatchedAtRealSeconds = 1'700'000'000.5;
        s.ignoreNudgeCount = 2;
        s.consecutivePollFailures = 1;
        return s;
    }

    // The module's state is process-wide, so each case starts from clean.
    struct FreshVisitState
    {
        FreshVisitState()
        {
            VisitState::Reset();
            VisitState::SetComposingSender(false);
            g_subPhase = VisitQuery::DiscussSubPhase::Discussing;
        }

        FreshVisitState(const FreshVisitState&) = delete;
        FreshVisitState& operator=(const FreshVisitState&) = delete;
    };
} // namespace

// The beat's own substate, which DerivePhase asks for while the quest sits at
// the Discuss stage.
namespace NarrativeEngine::NPCVisitBeat_Query
{
    DiscussSubPhase GetDiscussSubPhase()
    {
        return g_subPhase;
    }
} // namespace NarrativeEngine::NPCVisitBeat_Query

TEST_CASE("VisitState snapshot", "[VisitState][engine]")
{
    EngineMock engine;
    const FreshVisitState fresh;

    SECTION("when a snapshot is stored")
    {
        VisitState::SetSnapshot(PopulatedSnapshot());

        SECTION("should read back what was stored")
        {
            const auto s = VisitState::GetSnapshot();
            REQUIRE(s.senderFormID == kSenderFormID);
            REQUIRE(s.briefingText == "Ysolda wants to talk about the mammoth tusk.");
            REQUIRE(s.ignoreNudgeCount == 2);
        }
    }

    SECTION("when the state is reset")
    {
        VisitState::SetSnapshot(PopulatedSnapshot());
        VisitState::Reset();

        SECTION("should clear the return bookkeeping")
        {
            // The whole point of a reset: nothing about the previous visit may
            // survive into the next one, or the next sender is sent home to
            // somebody else's house.
            const auto s = VisitState::GetSnapshot();
            REQUIRE(s.senderFormID == 0);
            REQUIRE(s.returnCellFormID == 0);
            REQUIRE(s.returnAnchorFormID == 0);
        }

        SECTION("should clear the cached briefing")
        {
            const auto s = VisitState::GetSnapshot();
            REQUIRE(s.briefingText.empty());
            REQUIRE(s.narrationText.empty());
        }

        SECTION("should clear the counters")
        {
            REQUIRE(VisitState::GetSnapshot().ignoreNudgeCount == 0);
            REQUIRE(VisitState::GetSnapshot().consecutivePollFailures == 0);
        }
    }
}

TEST_CASE("VisitState history ring", "[VisitState][engine]")
{
    EngineMock engine;
    const FreshVisitState fresh;

    SECTION("when a visit ends")
    {
        VisitState::HistoryEntry entry;
        entry.senderName = "Ysolda";
        entry.topicTag = "debt";
        entry.outcome = Outcome::Unsatisfied;
        entry.durationSeconds = 91.5;
        VisitState::PushHistory(entry);

        SECTION("should record what happened")
        {
            // Checked at the back rather than by index: the ring is
            // process-wide and Reset deliberately leaves it alone, so what an
            // earlier case pushed is still in front of this entry.
            //
            // The outcome is the part that matters. Without it, a sender who
            // left because the player ignored them looks identical on the
            // dashboard to one whose conversation reached its end.
            const auto history = VisitState::GetHistory();
            REQUIRE_FALSE(history.empty());
            REQUIRE(history.back().senderName == "Ysolda");
            REQUIRE(history.back().outcome == Outcome::Unsatisfied);
            REQUIRE(history.back().durationSeconds == 91.5);
        }
    }

    SECTION("when more visits end than the ring holds")
    {
        for (std::size_t i = 0; i < VisitState::kHistoryRingSize + 4; ++i) {
            VisitState::HistoryEntry entry;
            entry.senderName = "sender" + std::to_string(i);
            VisitState::PushHistory(entry);
        }

        SECTION("should keep only the most recent")
        {
            const auto history = VisitState::GetHistory();
            REQUIRE(history.size() == VisitState::kHistoryRingSize);
        }

        SECTION("should drop the oldest rather than the newest")
        {
            // Trimming the wrong end would leave the dashboard permanently
            // showing the first ten visits of the save.
            const auto history = VisitState::GetHistory();
            REQUIRE(history.back().senderName == "sender13");
        }
    }

    SECTION("when the state is reset")
    {
        VisitState::HistoryEntry entry;
        entry.senderName = "Ysolda";
        VisitState::PushHistory(entry);
        const auto before = VisitState::GetHistory().size();
        VisitState::Reset();

        SECTION("should keep the history")
        {
            // Deliberate: Reset clears the in-flight visit, and it runs between
            // visits. Clearing the history with it would empty the dashboard's
            // recent list every time a visit finished.
            REQUIRE(VisitState::GetHistory().size() == before);
            REQUIRE(VisitState::GetHistory().back().senderName == "Ysolda");
        }
    }
}

TEST_CASE("VisitState::DerivePhase", "[VisitState][engine]")
{
    // The quest resolves once per process behind a call_once, and this suite
    // registers it, so every case here sees a resolved quest and drives the
    // phase through its stage.
    EngineMock engine;
    const FreshVisitState fresh;
    EngineMock::QuestState questState;
    questState.editorID = "_ne_VisitQuest";
    questState.formID = 0x000AB001u;
    REQUIRE(engine.AddQuest(questState) != nullptr);

    SECTION("when a compose call is in flight")
    {
        VisitState::SetComposingSender(true);

        SECTION("should report Composing rather than Idle")
        {
            // The quest has not started yet, so the stage says nothing. Without
            // this the dashboard shows Idle for the whole compose window, which
            // is the longest single step a visit has.
            engine.courier.questStage = 0;
            REQUIRE(VisitState::DerivePhase() == Mode::Composing);
        }

        SECTION("should outrank the quest's own stage")
        {
            engine.courier.questStage = 20;
            REQUIRE(VisitState::DerivePhase() == Mode::Composing);
        }
    }

    SECTION("when the quest is at a named stage")
    {
        SECTION("should map the salutation stage")
        {
            engine.courier.questStage = 10;
            REQUIRE(VisitState::DerivePhase() == Mode::Salutation);
        }

        SECTION("should map the valediction stage")
        {
            engine.courier.questStage = 30;
            REQUIRE(VisitState::DerivePhase() == Mode::Valediction);
        }

        SECTION("should map the return-home stage")
        {
            engine.courier.questStage = 50;
            REQUIRE(VisitState::DerivePhase() == Mode::ReturnHome);
        }
    }

    SECTION("when the quest is discussing")
    {
        engine.courier.questStage = 20;

        SECTION("should ask the beat which substate it is in")
        {
            // One stage, three displayed phases. The stage alone cannot tell
            // them apart, so the beat is the only source.
            g_subPhase = VisitQuery::DiscussSubPhase::OnHold;
            REQUIRE(VisitState::DerivePhase() == Mode::OnHold);
            g_subPhase = VisitQuery::DiscussSubPhase::ReEngage;
            REQUIRE(VisitState::DerivePhase() == Mode::ReEngage);
            g_subPhase = VisitQuery::DiscussSubPhase::Discussing;
            REQUIRE(VisitState::DerivePhase() == Mode::Discuss);
        }
    }

    SECTION("when the quest is tearing down")
    {
        SECTION("should read the rollback stage as idle")
        {
            // Teardown in progress. Displaying it as an active visit would
            // make the dashboard show a visit that has already ended, and the
            // next dispatch would look like it was refused for being busy.
            engine.courier.questStage = 60;
            REQUIRE(VisitState::DerivePhase() == Mode::Idle);
        }

        SECTION("should read the shutdown stage as idle")
        {
            engine.courier.questStage = 200;
            REQUIRE(VisitState::DerivePhase() == Mode::Idle);
        }

        SECTION("should read an unauthored stage as idle")
        {
            // Stage numbers are a contract with hand-authored Papyrus, so an
            // unknown one means the CK content and the C++ have drifted. Idle
            // is the safe reading: it lets the next dispatch through rather
            // than wedging the beat on a phase nothing will ever leave.
            engine.courier.questStage = 99;
            REQUIRE(VisitState::DerivePhase() == Mode::Idle);
        }
    }
}

TEST_CASE("VisitState survives a save and load", "[VisitState][engine]")
{
    // The round trip matters more than either half. The record's variable
    // strings sit after its scalars, so a writer and reader that disagree
    // about any one field read everything after it from the wrong offset.
    EngineMock engine;
    const FreshVisitState fresh;

    SECTION("when a populated snapshot is written and read back")
    {
        VisitState::SetSnapshot(PopulatedSnapshot());
        VisitState::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        VisitState::Reset();
        VisitState::OnLoad(FakeInterface(), 3, 0);
        const auto s = VisitState::GetSnapshot();

        SECTION("should restore the position it has to put the NPC back at")
        {
            // The fields that put an NPC back where they came from. Losing them
            // strands whoever the beat moved, permanently.
            REQUIRE(s.returnPosition.x == 100.5f);
            REQUIRE(s.returnPosition.y == -200.25f);
            REQUIRE(s.returnPosition.z == 300.125f);
            REQUIRE(s.returnAngleZ == 1.75f);
        }

        SECTION("should remap every form id through the load order")
        {
            // Not restored verbatim, and it must not be: a co-save records the
            // form ids of whatever load order wrote it, and the same NPC has a
            // different id after a plugin is added or removed. Reading them raw
            // would return the wrong NPC to the wrong cell. The mock resolves
            // everything to one sentinel, so seeing it here is proof each id
            // went through the resolver rather than round-tripping.
            REQUIRE(s.senderFormID == engine.cosave.resolvedFormID);
            REQUIRE(s.returnCellFormID == engine.cosave.resolvedFormID);
            REQUIRE(s.returnAnchorFormID == engine.cosave.resolvedFormID);
            REQUIRE(engine.cosave.resolveRequests.size() == 3);
        }

        SECTION("should clear an id the load order no longer has")
        {
            // A plugin the save depended on has been removed. Zero reads
            // downstream as "no anchor", which the beat handles; a stale id
            // would resolve to whatever form now occupies that slot.
            engine.cosave.readCursor = 0;
            engine.cosave.resolveSucceeds = false;
            VisitState::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(VisitState::GetSnapshot().senderFormID == 0);
        }

        SECTION("should restore every string")
        {
            REQUIRE(s.briefingText == "Ysolda wants to talk about the mammoth tusk.");
            REQUIRE(s.narrationText == "She approaches, wringing her hands.");
            REQUIRE(s.topicTag == "debt");
            REQUIRE(s.mood == "anxious");
        }

        SECTION("should restore the counters")
        {
            REQUIRE(s.ignoreNudgeCount == 2);
            REQUIRE(s.consecutivePollFailures == 1);
            REQUIRE(s.dispatchedAtRealSeconds == 1'700'000'000.5);
        }
    }

    SECTION("when the snapshot is empty")
    {
        VisitState::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        VisitState::SetSnapshot(PopulatedSnapshot());

        SECTION("should restore an empty snapshot rather than leave the old one")
        {
            // Saving between visits is the normal case, and the empty record
            // has to overwrite: otherwise a load carries the previous visit's
            // sender into a world that has no visit running.
            VisitState::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(VisitState::GetSnapshot().senderFormID == 0);
        }
    }

    SECTION("when the record stamps its type and version")
    {
        VisitState::OnSave(FakeInterface());

        SECTION("should use the frozen record type")
        {
            // Frozen by the header; changing it orphans every snapshot already
            // on disk, and the visits they described never end.
            REQUIRE(engine.cosave.opened.size() == 1);
            REQUIRE(engine.cosave.opened[0].type == VisitState::kRecordTypeId);
            REQUIRE(engine.cosave.opened[0].version == 3);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;
        VisitState::SetSnapshot(PopulatedSnapshot());

        SECTION("should write nothing")
        {
            VisitState::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing on either side")
        {
            VisitState::SetSnapshot(PopulatedSnapshot());
            VisitState::OnSave(nullptr);
            REQUIRE(engine.cosave.written.empty());
            VisitState::OnLoad(nullptr, 3, 0);
            REQUIRE(VisitState::GetSnapshot().senderFormID == kSenderFormID);
        }
    }
}

TEST_CASE("VisitState::OnLoad rejects a record it cannot trust", "[VisitState][engine]")
{
    // The live snapshot is populated first, so "cleared" is a state no case
    // could reach by accident.
    EngineMock engine;
    const FreshVisitState fresh;
    VisitState::SetSnapshot(PopulatedSnapshot());
    VisitState::OnSave(FakeInterface());
    const auto goodPayload = engine.cosave.written;

    SECTION("when the version is one no build ever wrote")
    {
        engine.cosave.readable = goodPayload;
        engine.cosave.readCursor = 0;

        SECTION("should clear the snapshot rather than misread it")
        {
            // The bytes are perfectly readable; only the version is wrong. The
            // three known versions differ by fields in the MIDDLE of the
            // record, so reading one as another misplaces everything after.
            VisitState::OnLoad(FakeInterface(), 9, 0);
            REQUIRE(VisitState::GetSnapshot().senderFormID == 0);
        }
    }

    SECTION("when the record ends inside the header")
    {
        engine.cosave.readable = goodPayload;
        engine.cosave.readable.resize(6);
        engine.cosave.readCursor = 0;

        SECTION("should clear the snapshot")
        {
            VisitState::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(VisitState::GetSnapshot().senderFormID == 0);
        }
    }

    SECTION("when the record ends before its strings")
    {
        engine.cosave.readable = goodPayload;
        engine.cosave.readable.resize(38);
        engine.cosave.readCursor = 0;

        SECTION("should clear the snapshot")
        {
            // A half-read snapshot is worse than none: it would name a sender
            // with no cell to return them to.
            VisitState::OnLoad(FakeInterface(), 3, 0);
            REQUIRE(VisitState::GetSnapshot().senderFormID == 0);
        }
    }

    SECTION("when the plugin reverts")
    {
        SECTION("should clear the snapshot")
        {
            VisitState::OnRevert();
            REQUIRE(VisitState::GetSnapshot().senderFormID == 0);
        }
    }
}
