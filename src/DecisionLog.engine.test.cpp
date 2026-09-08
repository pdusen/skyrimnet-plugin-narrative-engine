#include <DecisionLog.h>

#include <EngineMock.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Mocked-engine tests for the Director's decision ring buffer.
//
// The log is two things at once: the bounded record of every evaluation the
// Director has made, and the source of the tension score every NPC's prompt is
// rendered with. Both halves fail quietly. A cap that trims the wrong end loses
// the newest decisions while the log still looks full; a loader that accepts a
// half-read record restores whatever happened to be in memory next.
//
// The persistence half carries most of the risk, because the payload is
// variable-width. Each record holds three length-prefixed strings, so a reader
// that disagrees with the writer about any one field does not fail at that
// field — it resynchronises onto the wrong offset and reads the rest of the
// save as record data. Every rejection path below therefore checks that the
// whole payload was discarded, not that one record was skipped.
//
// One repair is deliberately part of the loader rather than of the writer:
// notes saved by builds before the byte-boundary fix in TruncateUTF8 can end
// mid-character, and left alone they throw out of every dump() that touches the
// log. That is asserted here because the affected saves still exist.

namespace
{
    using NarrativeEngine::Testing::EngineMock;
    namespace DecisionLog = NarrativeEngine::DecisionLog;
    namespace PhaseTracker = NarrativeEngine::PhaseTracker;
    using DecisionLog::DecisionRecord;
    using PhaseTracker::Phase;

    // A non-null interface pointer; the mocked SKSE methods answer out of
    // EngineMock and never read through it.
    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    // The log is process-wide, so every case starts from empty and with the
    // default cap restored. Declared as a local so it re-runs per leaf path —
    // RAII in place of a reset hook.
    struct FreshLog
    {
        static constexpr std::size_t kDefaultMaxEntries = 200;

        FreshLog()
        {
            DecisionLog::SetMaxEntries(kDefaultMaxEntries);
            DecisionLog::Clear();
        }

        FreshLog(const FreshLog&) = delete;
        FreshLog& operator=(const FreshLog&) = delete;
    };

    // A record with every field set to something distinctive, so a writer or
    // reader that transposes two fields produces a visible mismatch rather than
    // two zeroes that happen to agree.
    DecisionRecord SampleRecord(std::uint32_t tension)
    {
        DecisionRecord r;
        r.realTimeSec = 1'700'000'000.5;
        r.gameDaysPassed = 12.25f;
        r.tensionScore = tension;
        r.currentPhase = Phase::RisingAction;
        r.advancedToPhase = Phase::Climax;
        r.beatSelected = "NPCVisit";
        r.beatParametersJSON = R"({"sender":"Ysolda"})";
        r.narrativeNote = "The city grows uneasy.";
        r.alphaCanonActiveSignals = 0x5u;
        return r;
    }

    template <class T> void Append(std::vector<std::byte>& out, const T& value)
    {
        const auto* first = reinterpret_cast<const std::byte*>(&value);
        out.insert(out.end(), first, first + sizeof(T));
    }

    void AppendString(std::vector<std::byte>& out, const std::string& s)
    {
        Append(out, static_cast<std::uint32_t>(s.size()));
        const auto* first = reinterpret_cast<const std::byte*>(s.data());
        out.insert(out.end(), first, first + s.size());
    }

    // Builds one record's bytes by hand, so the loader is fed a payload rather
    // than whatever the writer happens to produce. The phase bytes are taken
    // raw so a case can put a value in them that no Phase has.
    void AppendRecord(std::vector<std::byte>& out,
                      std::uint32_t tension,
                      std::uint8_t currentPhaseByte,
                      std::uint8_t hasAdvanced,
                      std::uint8_t advancedByte,
                      const std::string& note)
    {
        Append(out, 1'700'000'000.5);
        Append(out, 12.25f);
        Append(out, tension);
        Append(out, currentPhaseByte);
        Append(out, hasAdvanced);
        Append(out, advancedByte);
        AppendString(out, "NPCVisit");
        AppendString(out, R"({"sender":"Ysolda"})");
        AppendString(out, note);
        Append(out, static_cast<std::uint32_t>(0x5u));
    }

    // A one-record payload with the count header in front.
    std::vector<std::byte> OneRecordPayload(
        std::uint32_t tension = 42,
        std::uint8_t currentPhaseByte = static_cast<std::uint8_t>(Phase::RisingAction),
        std::uint8_t hasAdvanced = 1,
        std::uint8_t advancedByte = static_cast<std::uint8_t>(Phase::Climax),
        const std::string& note = "The city grows uneasy.")
    {
        std::vector<std::byte> payload;
        Append(payload, static_cast<std::uint32_t>(1));
        AppendRecord(payload, tension, currentPhaseByte, hasAdvanced, advancedByte, note);
        return payload;
    }
} // namespace

TEST_CASE("DecisionLog::Append", "[DecisionLog][engine]")
{
    const FreshLog log;

    SECTION("when a decision is recorded")
    {
        DecisionLog::Append(SampleRecord(42));

        SECTION("should be readable back")
        {
            const auto tail = DecisionLog::Tail(1);
            REQUIRE(tail.size() == 1);
            REQUIRE(tail[0].tensionScore == 42);
            REQUIRE(tail[0].beatSelected == "NPCVisit");
        }
    }

    SECTION("when the log is over the cap")
    {
        DecisionLog::SetMaxEntries(3);
        for (std::uint32_t i = 0; i < 5; ++i)
            DecisionLog::Append(SampleRecord(i));

        SECTION("should drop the oldest and keep the newest")
        {
            // Which end is trimmed is the whole point. Popping the back would
            // leave the log looking healthy while permanently pinning the
            // Director's view of the world to whatever happened first.
            const auto tail = DecisionLog::Tail(10);
            REQUIRE(tail.size() == 3);
            REQUIRE(tail.front().tensionScore == 2);
            REQUIRE(tail.back().tensionScore == 4);
        }
    }
}

TEST_CASE("DecisionLog::Tail", "[DecisionLog][engine]")
{
    // Happy path, re-run per leaf: four decisions with ascending scores, so the
    // order the tail comes back in is visible rather than inferred.
    const FreshLog log;
    for (std::uint32_t i = 0; i < 4; ++i)
        DecisionLog::Append(SampleRecord(i));

    SECTION("when fewer records are asked for than exist")
    {
        SECTION("should return the newest, oldest first")
        {
            // Oldest-first within a newest-N window: the prompt reads the tail
            // as a narrative in the order it happened, but only the recent part
            // of it.
            const auto tail = DecisionLog::Tail(2);
            REQUIRE(tail.size() == 2);
            REQUIRE(tail[0].tensionScore == 2);
            REQUIRE(tail[1].tensionScore == 3);
        }
    }

    SECTION("when more records are asked for than exist")
    {
        SECTION("should return everything")
        {
            const auto tail = DecisionLog::Tail(100);
            REQUIRE(tail.size() == 4);
            REQUIRE(tail.front().tensionScore == 0);
        }
    }

    SECTION("when none are asked for")
    {
        SECTION("should return nothing")
        {
            REQUIRE(DecisionLog::Tail(0).empty());
        }
    }

    SECTION("when the log is empty")
    {
        DecisionLog::Clear();

        SECTION("should return nothing")
        {
            REQUIRE(DecisionLog::Tail(5).empty());
        }
    }
}

TEST_CASE("DecisionLog::SetMaxEntries", "[DecisionLog][engine]")
{
    const FreshLog log;
    for (std::uint32_t i = 0; i < 5; ++i)
        DecisionLog::Append(SampleRecord(i));

    SECTION("when the cap is lowered below the current size")
    {
        SECTION("should trim immediately")
        {
            // Wired from settings at kDataLoaded, after the log may already
            // have been restored from the co-save — so the trim has to happen
            // on the way in, not only on the next append.
            DecisionLog::SetMaxEntries(2);
            const auto tail = DecisionLog::Tail(100);
            REQUIRE(tail.size() == 2);
            REQUIRE(tail.front().tensionScore == 3);
        }
    }

    SECTION("when the cap is raised")
    {
        SECTION("should keep everything")
        {
            DecisionLog::SetMaxEntries(500);
            REQUIRE(DecisionLog::Tail(100).size() == 5);
        }
    }
}

TEST_CASE("DecisionLog::LatestTensionScore", "[DecisionLog][engine]")
{
    const FreshLog log;

    SECTION("when decisions have been recorded")
    {
        DecisionLog::Append(SampleRecord(10));
        DecisionLog::Append(SampleRecord(77));

        SECTION("should report the most recent score")
        {
            // Backs the ne_narrative_tension decorator, which is rendered into
            // every NPC's prompt. Reading the front would hold the whole world
            // at the mood of the first evaluation of the save.
            REQUIRE(DecisionLog::LatestTensionScore() == 77u);
        }
    }

    SECTION("when the log is empty")
    {
        SECTION("should report nothing")
        {
            // The decorator maps this to "0" itself; reporting a bare 0 from
            // here would make "calm" and "never evaluated" indistinguishable.
            REQUIRE_FALSE(DecisionLog::LatestTensionScore().has_value());
        }
    }
}

TEST_CASE("DecisionLog::Clear", "[DecisionLog][engine]")
{
    const FreshLog log;
    DecisionLog::Append(SampleRecord(42));

    SECTION("when the log is cleared")
    {
        SECTION("should drop every record")
        {
            DecisionLog::Clear();
            REQUIRE(DecisionLog::Tail(100).empty());
            REQUIRE_FALSE(DecisionLog::LatestTensionScore().has_value());
        }
    }

    SECTION("when the plugin reverts")
    {
        SECTION("should drop every record")
        {
            // Records belong to one save. Carrying them into the next one would
            // have the Director reasoning about a story that never happened in
            // the world the player just loaded.
            DecisionLog::OnRevert();
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }
}

TEST_CASE("DecisionLog::OnSave", "[DecisionLog][engine]")
{
    // Happy path, re-run per leaf: one decision in the log and a co-save that
    // accepts writes.
    EngineMock engine;
    const FreshLog log;
    DecisionLog::Append(SampleRecord(42));

    SECTION("when the record opens")
    {
        DecisionLog::OnSave(FakeInterface());

        SECTION("should stamp the frozen record type and version")
        {
            // The type is frozen by the header; changing it orphans every
            // payload already on disk. The version gates the loader's layout.
            REQUIRE(engine.cosave.opened.size() == 1);
            REQUIRE(engine.cosave.opened[0].type == DecisionLog::kRecordTypeId);
            REQUIRE(engine.cosave.opened[0].version == 1);
        }

        SECTION("should lead with the entry count")
        {
            // Every record after the header is variable-width, so the count is
            // the only thing telling the reader when to stop.
            std::uint32_t count = 0;
            REQUIRE(engine.cosave.written.size() >= sizeof(count));
            std::memcpy(&count, engine.cosave.written.data(), sizeof(count));
            REQUIRE(count == 1);
        }
    }

    SECTION("when the record will not open")
    {
        engine.cosave.openRecordSucceeds = false;

        SECTION("should write nothing")
        {
            // Writing into a record that was never opened corrupts whichever
            // record the co-save is actually positioned in.
            DecisionLog::OnSave(FakeInterface());
            REQUIRE(engine.cosave.written.empty());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing")
        {
            DecisionLog::OnSave(nullptr);
            REQUIRE(engine.cosave.opened.empty());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}

TEST_CASE("DecisionLog survives a save and load", "[DecisionLog][engine]")
{
    // Happy path, re-run per leaf: one fully-populated decision written out and
    // read back. The round trip matters more than either half — a writer and a
    // reader that disagree about one field resynchronise onto the wrong offset
    // and misread everything after it, and the damage surfaces on load.
    EngineMock engine;
    const FreshLog log;

    SECTION("when a decision is written and read back")
    {
        DecisionLog::Append(SampleRecord(42));
        DecisionLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        DecisionLog::Clear();
        DecisionLog::OnLoad(FakeInterface(), 1, 0);
        const auto tail = DecisionLog::Tail(100);
        REQUIRE(tail.size() == 1);

        SECTION("should restore every scalar field")
        {
            REQUIRE(tail[0].realTimeSec == 1'700'000'000.5);
            REQUIRE(tail[0].gameDaysPassed == 12.25f);
            REQUIRE(tail[0].tensionScore == 42u);
            REQUIRE(tail[0].currentPhase == Phase::RisingAction);
            REQUIRE(tail[0].alphaCanonActiveSignals == 0x5u);
        }

        SECTION("should restore every string field")
        {
            REQUIRE(tail[0].beatSelected == "NPCVisit");
            REQUIRE(tail[0].beatParametersJSON == R"({"sender":"Ysolda"})");
            REQUIRE(tail[0].narrativeNote == "The city grows uneasy.");
        }

        SECTION("should restore the phase advancement")
        {
            REQUIRE(tail[0].advancedToPhase == Phase::Climax);
        }
    }

    SECTION("when the decision recorded no advancement")
    {
        auto record = SampleRecord(42);
        record.advancedToPhase.reset();
        DecisionLog::Append(record);
        DecisionLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        DecisionLog::Clear();

        SECTION("should restore it as no advancement")
        {
            // The value byte is written either way to keep the per-record
            // header fixed-width, so only the flag distinguishes "stayed put"
            // from "advanced to Exposition" — which is a real transition out of
            // Resolution.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE_FALSE(DecisionLog::Tail(1)[0].advancedToPhase.has_value());
        }
    }

    SECTION("when several decisions are written")
    {
        for (std::uint32_t i = 0; i < 4; ++i)
            DecisionLog::Append(SampleRecord(i));
        DecisionLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        DecisionLog::Clear();

        SECTION("should restore them in order")
        {
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            const auto tail = DecisionLog::Tail(100);
            REQUIRE(tail.size() == 4);
            REQUIRE(tail[0].tensionScore == 0u);
            REQUIRE(tail[3].tensionScore == 3u);
        }
    }

    SECTION("when the log was empty")
    {
        DecisionLog::OnSave(FakeInterface());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        DecisionLog::Append(SampleRecord(42));

        SECTION("should restore an empty log")
        {
            // A zero count is a valid payload, not a short read, so loading it
            // has to replace the live log rather than leave it alone.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }
}

TEST_CASE("DecisionLog::OnLoad repairs what it can", "[DecisionLog][engine]")
{
    // Happy path, re-run per leaf: a hand-built one-record payload that is
    // well-formed except for whatever the case corrupts. Built by hand rather
    // than through OnSave so a case can put a byte on the wire that the writer
    // would never produce.
    EngineMock engine;
    const FreshLog log;

    SECTION("when a record names a phase that does not exist")
    {
        engine.cosave.readable = OneRecordPayload(42, 9);
        engine.cosave.readCursor = 0;

        SECTION("should clamp it rather than discard the payload")
        {
            // The phase byte reaches a name-table index and a threshold switch,
            // both of which degrade quietly, so a bad value would go unnoticed
            // rather than crash. Clamping keeps the rest of the record, which
            // is still worth having.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            const auto tail = DecisionLog::Tail(100);
            REQUIRE(tail.size() == 1);
            REQUIRE(tail[0].currentPhase == Phase::Exposition);
            REQUIRE(tail[0].tensionScore == 42u);
        }
    }

    SECTION("when a record advances to a phase that does not exist")
    {
        engine.cosave.readable = OneRecordPayload(42, static_cast<std::uint8_t>(Phase::RisingAction), 1, 9);
        engine.cosave.readCursor = 0;

        SECTION("should keep the record and drop the advancement")
        {
            // Dropping to nullopt rather than clamping to Exposition: an
            // advancement that cannot be trusted is better recorded as none
            // than as a transition that never happened.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            const auto tail = DecisionLog::Tail(100);
            REQUIRE(tail.size() == 1);
            REQUIRE_FALSE(tail[0].advancedToPhase.has_value());
        }
    }

    SECTION("when a note ends mid-character")
    {
        // "caf" plus a lone UTF-8 lead byte, which is what a build predating
        // the byte-boundary fix in TruncateUTF8 could leave behind.
        engine.cosave.readable =
            OneRecordPayload(42, static_cast<std::uint8_t>(Phase::RisingAction), 1, 0, std::string{"caf\xC3"});
        engine.cosave.readCursor = 0;

        SECTION("should repair it before it reaches the log")
        {
            // Not cosmetic. nlohmann refuses to serialize invalid UTF-8, so an
            // unrepaired note throws out of every dump() that touches the log —
            // every Director tick and every dashboard push — leaving the save
            // broken even on a build that has the writer fix.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(DecisionLog::Tail(1)[0].narrativeNote == "caf");
        }
    }

    SECTION("when the payload holds more records than the cap allows")
    {
        std::vector<std::byte> payload;
        Append(payload, static_cast<std::uint32_t>(4));
        for (std::uint32_t i = 0; i < 4; ++i)
            AppendRecord(payload, i, static_cast<std::uint8_t>(Phase::RisingAction), 0, 0, "note");
        engine.cosave.readable = payload;
        engine.cosave.readCursor = 0;
        DecisionLog::SetMaxEntries(2);

        SECTION("should trim to the cap")
        {
            // A save written before the cap was lowered still has to come back
            // within it, and the newest records are the ones to keep.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            const auto tail = DecisionLog::Tail(100);
            REQUIRE(tail.size() == 2);
            REQUIRE(tail.front().tensionScore == 2u);
        }
    }
}

TEST_CASE("DecisionLog::OnLoad rejects a payload it cannot trust", "[DecisionLog][engine]")
{
    // Happy path, re-run per leaf: the live log holds a decision before the
    // load, so "discarded the payload" is distinguishable from "loaded nothing
    // and left what was there".
    EngineMock engine;
    const FreshLog log;
    DecisionLog::Append(SampleRecord(99));

    SECTION("when the version is one no build ever wrote")
    {
        engine.cosave.readable = OneRecordPayload();
        engine.cosave.readCursor = 0;

        SECTION("should discard the whole payload")
        {
            // The bytes are perfectly readable; only the version is wrong.
            // Reading them anyway would restore fields from the wrong offsets.
            DecisionLog::OnLoad(FakeInterface(), 7, 0);
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }

    SECTION("when the record ends before the entry count")
    {
        SECTION("should discard the whole payload")
        {
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }

    SECTION("when a record's fixed fields are truncated")
    {
        engine.cosave.readable = OneRecordPayload();
        engine.cosave.readable.resize(sizeof(std::uint32_t) + 4);
        engine.cosave.readCursor = 0;

        SECTION("should discard the whole payload")
        {
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }

    SECTION("when a record's string is truncated")
    {
        engine.cosave.readable = OneRecordPayload();
        // Past the fixed fields and the first length prefix, into the text.
        engine.cosave.readable.resize(sizeof(std::uint32_t) + 19 + sizeof(std::uint32_t) + 3);
        engine.cosave.readCursor = 0;

        SECTION("should discard the whole payload")
        {
            // The sharpest case. A reader that shrugged at a short string would
            // carry on at an offset the writer never wrote to, and every later
            // length prefix would be read out of the middle of some other
            // field.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }

    SECTION("when the trailing signal mask is missing")
    {
        engine.cosave.readable = OneRecordPayload();
        engine.cosave.readable.resize(engine.cosave.readable.size() - sizeof(std::uint32_t));
        engine.cosave.readCursor = 0;

        SECTION("should discard the whole payload")
        {
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }

    SECTION("when the count claims more records than the payload holds")
    {
        auto payload = OneRecordPayload();
        const auto claimed = static_cast<std::uint32_t>(5);
        std::memcpy(payload.data(), &claimed, sizeof(claimed));
        engine.cosave.readable = payload;
        engine.cosave.readCursor = 0;

        SECTION("should discard the whole payload")
        {
            // The one good record is thrown away with the rest. That is the
            // right trade: a count that disagrees with the bytes means the
            // record boundaries are not where the reader thinks they are, so
            // the first record is no more trustworthy than the missing ones.
            DecisionLog::OnLoad(FakeInterface(), 1, 0);
            REQUIRE(DecisionLog::Tail(100).empty());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should leave the live log alone")
        {
            // Distinct from every rejection above: with no interface there is
            // nothing to have gone wrong, so clearing would discard good state
            // rather than protect anything.
            DecisionLog::OnLoad(nullptr, 1, 0);
            REQUIRE(DecisionLog::Tail(100).size() == 1);
        }
    }
}
