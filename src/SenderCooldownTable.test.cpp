#include <SenderCooldownTable.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Unit tests for the per-sender cooldown / watermark table.
//
// This module is the interesting case for the co-located test convention,
// because unlike LLMTextSanitizer it is not pure and never could be: it stamps
// against the game clock and it round-trips through the SKSE co-save. Neither
// dependency can be refactored into a pure function, so instead they are
// injected — the clock as an ordinary `double` parameter, the co-save behind
// the `ICosaveIO` port. That is what lets these tests run the REAL production
// class, not a reimplementation of it, with no Skyrim and no SKSE.
//
// The fake below is what makes the co-save cases reachable at all. A torn save,
// a mod removed from the load order, a form that resolves to zero — every one
// of those is a support ticket in the wild and a one-line setup here.

namespace
{
    using NarrativeEngine::ICosaveIO;
    using NarrativeEngine::SenderCooldownTable;
    using FormID = SenderCooldownTable::FormID;

    // In-memory stand-in for the SKSE co-save stream.
    //
    // Byte-accurate on purpose: it holds a real buffer with a real read cursor
    // and returns short counts at the end of it, exactly as
    // `SKSE::SerializationInterface::ReadRecordData` does. A fake that returned
    // whole values instead of byte counts would quietly stop testing the
    // short-read handling, which is most of what Deserialize does.
    //
    // FormID resolution defaults to identity — the everyday case where the load
    // order has not changed. `RemoveFromLoadOrder` and `RemapFormID` express the
    // two ways that assumption breaks.
    class FakeCosave final : public ICosaveIO
    {
    public:
        bool WriteBytes(const void* data, std::uint32_t length) override
        {
            const auto* first = static_cast<const std::byte*>(data);
            buffer_.insert(buffer_.end(), first, first + length);
            return true;
        }

        std::uint32_t ReadBytes(void* out, std::uint32_t length) override
        {
            const auto remaining = static_cast<std::uint32_t>(buffer_.size() - readCursor_);
            const auto n = length < remaining ? length : remaining;
            if (n > 0) {
                std::memcpy(out, buffer_.data() + readCursor_, n);
                readCursor_ += n;
            }
            return n;
        }

        bool ResolveFormID(std::uint32_t oldFormID, std::uint32_t& newFormID) const override
        {
            if (unresolvable_.contains(oldFormID))
                return false;
            const auto it = remapped_.find(oldFormID);
            newFormID = it != remapped_.end() ? it->second : oldFormID;
            return true;
        }

        // Rewind so a table can read back what another table wrote — the same
        // stream position a load callback starts from.
        void RewindForReading()
        {
            readCursor_ = 0;
        }

        // Chop the tail off the written record. Models a co-save truncated by a
        // crash mid-write, or a record whose writer stopped early.
        void TruncateTo(std::size_t bytes)
        {
            if (bytes < buffer_.size())
                buffer_.resize(bytes);
        }

        // This form's owning plugin is gone from the load order; SKSE fails to
        // resolve it and the entry must be dropped.
        void RemoveFromLoadOrder(FormID formID)
        {
            unresolvable_.insert(formID);
        }

        // This form still exists but its plugin moved in the load order, so it
        // now carries a different FormID.
        void RemapFormID(FormID oldFormID, FormID newFormID)
        {
            remapped_[oldFormID] = newFormID;
        }

        std::size_t BytesWritten() const
        {
            return buffer_.size();
        }

    private:
        std::vector<std::byte> buffer_;
        std::size_t readCursor_ = 0;
        std::unordered_map<FormID, FormID> remapped_;
        std::unordered_set<FormID> unresolvable_;
    };

    // Two arbitrary but distinct senders, and a clock reading that is not zero
    // — zero is a sentinel the table treats as "never stamped", so using a real
    // hour value keeps the ordinary cases away from that edge.
    constexpr FormID kFaralda = 0x0001A6A0;
    constexpr FormID kYsolda = 0x0001A6A1;
    constexpr double kNoon = 100.0;
} // namespace

TEST_CASE("SenderCooldownTable cooldown window", "[SenderCooldownTable]")
{
    // Common setup, rebuilt for every leaf SECTION: an empty table and a
    // 24-hour cooldown, the shape both beats use. Destroyed and reconstructed
    // between paths, so no section can see another's stamps — the RAII
    // equivalent of a reset hook, without one.
    SenderCooldownTable table;
    constexpr int kCooldownHours = 24;

    SECTION("when the sender has never been stamped")
    {
        SECTION("should not be on cooldown")
        {
            REQUIRE_FALSE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon));
        }

        SECTION("should report no stamp at all")
        {
            REQUIRE_FALSE(table.GetStampGameHours(kFaralda).has_value());
        }
    }

    SECTION("when the sender was stamped")
    {
        table.Stamp(kFaralda, kNoon);

        SECTION("and no time has passed")
        {
            SECTION("should be on cooldown")
            {
                REQUIRE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon));
            }

            SECTION("should report the stamp it was given")
            {
                REQUIRE(table.GetStampGameHours(kFaralda) == kNoon);
            }
        }

        SECTION("and part of the window has elapsed")
        {
            SECTION("should still be on cooldown")
            {
                REQUIRE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon + 23.9));
            }
        }

        SECTION("and exactly the full window has elapsed")
        {
            // The comparison is `elapsed < cooldownHours`, so the boundary hour
            // itself is already free. Pinned because an off-by-one here is the
            // difference between a 24-hour cooldown and a 25-hour one, and
            // nothing in the game would make that visible.
            SECTION("should no longer be on cooldown")
            {
                REQUIRE_FALSE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon + 24.0));
            }
        }

        SECTION("and more than the window has elapsed")
        {
            SECTION("should no longer be on cooldown")
            {
                REQUIRE_FALSE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon + 100.0));
            }
        }

        SECTION("and a different sender is asked about")
        {
            SECTION("should not be on cooldown")
            {
                REQUIRE_FALSE(table.IsOnCooldown(kYsolda, kCooldownHours, kNoon));
            }
        }

        SECTION("and the sender is stamped again later")
        {
            table.Stamp(kFaralda, kNoon + 100.0);

            SECTION("should restart the window from the newer stamp")
            {
                REQUIRE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon + 110.0));
            }

            SECTION("should report the newer stamp")
            {
                REQUIRE(table.GetStampGameHours(kFaralda) == kNoon + 100.0);
            }
        }

        SECTION("and the table is cleared")
        {
            table.Clear();

            SECTION("should not be on cooldown")
            {
                REQUIRE_FALSE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon));
            }

            SECTION("should report no stamp")
            {
                REQUIRE_FALSE(table.GetStampGameHours(kFaralda).has_value());
            }
        }

        SECTION("and the player reloads a save from before the stamp")
        {
            // The clock runs backwards across a save load, so `elapsed` goes
            // negative and stays under any positive window. The sender is held
            // on cooldown until game time catches back up — deliberate, and
            // the safe direction to fail: the alternative is a sender freed
            // early every time the player reloads.
            SECTION("should keep the sender on cooldown")
            {
                REQUIRE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon - 50.0));
            }
        }
    }

    SECTION("when the cooldown is configured off")
    {
        table.Stamp(kFaralda, kNoon);

        SECTION("and the setting is zero hours")
        {
            SECTION("should never report a cooldown")
            {
                REQUIRE_FALSE(table.IsOnCooldown(kFaralda, 0, kNoon));
            }
        }

        SECTION("and the setting is negative")
        {
            SECTION("should never report a cooldown")
            {
                REQUIRE_FALSE(table.IsOnCooldown(kFaralda, -5, kNoon));
            }
        }

        SECTION("should still keep the stamp for the watermark caller")
        {
            // Both beats reuse one table shape for two jobs: the cooldown
            // filter and the memory watermark. Turning the cooldown off must
            // not blind the watermark path, which reads the stamp directly.
            REQUIRE(table.GetStampGameHours(kFaralda) == kNoon);
        }
    }

    SECTION("when the sender FormID is zero")
    {
        // A null form reaches here whenever an upstream lookup failed. It must
        // not become a map key: FormID 0 is not a sender, and one bogus entry
        // would then answer for every failed lookup that followed.
        table.Stamp(0, kNoon);

        SECTION("should not be recorded")
        {
            REQUIRE_FALSE(table.GetStampGameHours(0).has_value());
        }

        SECTION("should never report a cooldown")
        {
            REQUIRE_FALSE(table.IsOnCooldown(0, kCooldownHours, kNoon));
        }
    }

    SECTION("when a stamp of zero hours was recorded")
    {
        // Zero is what an unavailable Calendar returns, so it is treated as
        // "no usable stamp" by the cooldown filter even though the entry
        // exists. The watermark caller still sees the entry, since for it a
        // zero watermark simply excludes nothing.
        table.Stamp(kFaralda, 0.0);

        SECTION("should not report a cooldown")
        {
            REQUIRE_FALSE(table.IsOnCooldown(kFaralda, kCooldownHours, kNoon));
        }

        SECTION("should still report the entry exists")
        {
            REQUIRE(table.GetStampGameHours(kFaralda) == 0.0);
        }
    }
}

TEST_CASE("SenderCooldownTable co-save round trip", "[SenderCooldownTable][cosave]")
{
    // Common setup: a fake co-save stream, plus the table that will write into
    // it and a second table that will read it back. Two separate tables rather
    // than one, because a save and the load that follows it happen in different
    // process lifetimes — reusing one object would let stale in-memory state
    // stand in for data the load was supposed to restore.
    FakeCosave cosave;
    SenderCooldownTable saved;
    SenderCooldownTable loaded;

    SECTION("when the table holds entries")
    {
        saved.Stamp(kFaralda, kNoon);
        saved.Stamp(kYsolda, kNoon + 12.0);
        saved.Serialize(cosave);
        cosave.RewindForReading();

        SECTION("and the load order is unchanged")
        {
            const bool ok = loaded.Deserialize(cosave);

            SECTION("should report success")
            {
                REQUIRE(ok);
            }

            SECTION("should restore every stamp exactly")
            {
                REQUIRE(loaded.GetStampGameHours(kFaralda) == kNoon);
                REQUIRE(loaded.GetStampGameHours(kYsolda) == kNoon + 12.0);
            }

            SECTION("should restore cooldown behaviour, not just the numbers")
            {
                REQUIRE(loaded.IsOnCooldown(kFaralda, 24, kNoon + 1.0));
                REQUIRE_FALSE(loaded.IsOnCooldown(kFaralda, 24, kNoon + 25.0));
            }
        }

        SECTION("and a sender's plugin has been removed from the load order")
        {
            // The scenario the ResolveFormID call exists for. Keeping the raw
            // FormID would be worse than dropping it: that ID now belongs to
            // whatever plugin slid into the vacated load-order slot, so the
            // cooldown would silently attach to an unrelated NPC.
            cosave.RemoveFromLoadOrder(kFaralda);
            const bool ok = loaded.Deserialize(cosave);

            SECTION("should still report success")
            {
                REQUIRE(ok);
            }

            SECTION("should drop the unresolvable sender")
            {
                REQUIRE_FALSE(loaded.GetStampGameHours(kFaralda).has_value());
            }

            SECTION("should keep the senders that still resolve")
            {
                REQUIRE(loaded.GetStampGameHours(kYsolda) == kNoon + 12.0);
            }
        }

        SECTION("and a sender's plugin has moved in the load order")
        {
            constexpr FormID kFaraldaRelocated = 0x0501A6A0;
            cosave.RemapFormID(kFaralda, kFaraldaRelocated);
            REQUIRE(loaded.Deserialize(cosave));

            SECTION("should file the stamp under the new FormID")
            {
                REQUIRE(loaded.GetStampGameHours(kFaraldaRelocated) == kNoon);
            }

            SECTION("should not keep the stamp under the old FormID")
            {
                REQUIRE_FALSE(loaded.GetStampGameHours(kFaralda).has_value());
            }
        }

        SECTION("and the record was truncated after the count")
        {
            // A save torn mid-write: the header promises two entries and the
            // bytes for them are gone.
            cosave.TruncateTo(sizeof(std::uint32_t));
            loaded.Stamp(kYsolda, kNoon);
            const bool ok = loaded.Deserialize(cosave);

            SECTION("should report failure")
            {
                REQUIRE_FALSE(ok);
            }

            SECTION("should leave no half-loaded state behind")
            {
                // Failing loud AND empty is the point. A partly-populated table
                // would look successful to every later caller, and the beats
                // decide whether to bother an NPC from exactly this data.
                REQUIRE_FALSE(loaded.GetStampGameHours(kYsolda).has_value());
                REQUIRE_FALSE(loaded.GetStampGameHours(kFaralda).has_value());
            }
        }

        SECTION("and the record was truncated inside an entry's stamp")
        {
            // Cut four bytes off the tail, so the final entry's FormID reads
            // fine and its double comes up short — the read that has to fail is
            // the second half of a field, not a whole one.
            cosave.TruncateTo(cosave.BytesWritten() - 4);

            SECTION("should report failure")
            {
                REQUIRE_FALSE(loaded.Deserialize(cosave));
            }
        }
    }

    SECTION("when the table is empty")
    {
        saved.Serialize(cosave);
        cosave.RewindForReading();

        SECTION("should write only the count")
        {
            REQUIRE(cosave.BytesWritten() == sizeof(std::uint32_t));
        }

        SECTION("should load back as an empty table")
        {
            REQUIRE(loaded.Deserialize(cosave));
            REQUIRE_FALSE(loaded.GetStampGameHours(kFaralda).has_value());
        }
    }

    SECTION("when the record is missing entirely")
    {
        // Nothing was ever written, so even the count comes up short. This is
        // what a beat sees when it reads past the end of its own record.
        SECTION("should report failure")
        {
            REQUIRE_FALSE(loaded.Deserialize(cosave));
        }
    }

    SECTION("when a load follows an earlier load")
    {
        saved.Stamp(kFaralda, kNoon);
        saved.Serialize(cosave);
        cosave.RewindForReading();
        loaded.Stamp(kYsolda, kNoon + 5.0);
        REQUIRE(loaded.Deserialize(cosave));

        SECTION("should replace the previous contents rather than merge")
        {
            // Loading a save has to install that save's state, not add to
            // whatever the last one left. A merge here would leak cooldowns
            // between saves for anyone who loads twice without quitting.
            REQUIRE(loaded.GetStampGameHours(kFaralda) == kNoon);
            REQUIRE_FALSE(loaded.GetStampGameHours(kYsolda).has_value());
        }
    }

    SECTION("when two tables share one record, as the beats write them")
    {
        // NPCLetterBeat and NPCVisitBeat both write a cooldown table and a
        // watermark table back-to-back into a single record, then read them
        // back in the same order. Each table has to consume exactly its own
        // payload; one byte of drift would feed the second table the first
        // one's tail.
        SenderCooldownTable savedWatermarks;
        saved.Stamp(kFaralda, kNoon);
        savedWatermarks.Stamp(kYsolda, kNoon + 7.0);
        saved.Serialize(cosave);
        savedWatermarks.Serialize(cosave);
        cosave.RewindForReading();

        SenderCooldownTable loadedWatermarks;
        const bool firstOk = loaded.Deserialize(cosave);
        const bool secondOk = loadedWatermarks.Deserialize(cosave);

        SECTION("should load both tables")
        {
            REQUIRE(firstOk);
            REQUIRE(secondOk);
        }

        SECTION("should give each table only its own entries")
        {
            REQUIRE(loaded.GetStampGameHours(kFaralda) == kNoon);
            REQUIRE_FALSE(loaded.GetStampGameHours(kYsolda).has_value());
            REQUIRE(loadedWatermarks.GetStampGameHours(kYsolda) == kNoon + 7.0);
            REQUIRE_FALSE(loadedWatermarks.GetStampGameHours(kFaralda).has_value());
        }
    }
}
