#pragma once

#include <CosaveIO.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace NarrativeEngine
{
    // SenderCooldownTable — the per-sender in-game-hours cooldown
    // pattern shared by beats that space out how often a specific NPC
    // can be re-picked as a beat sender (currently NPCLetterBeat's
    // per-sender delivery cooldown and NPCVisitBeat's per-sender visit
    // cooldown).
    //
    // Storage is a FormID -> game-hours stamp map guarded by an
    // internal mutex. `Stamp()` records the current in-game hours
    // when a beat's semantic "sender was consumed" event fires (letter
    // = courier hand-off; visit = arrival + opening line).
    // `IsOnCooldown()` is the compose-time filter: "has enough time
    // passed since this sender was last used?"
    //
    // Cosave layout, written by Serialize (caller has already opened
    // the record) and read by Deserialize:
    //     u32 count
    //     [FormID u32 + stamp double] * count
    // FormIDs pass through ICosaveIO::ResolveFormID on load; entries whose
    // owner-mod is no longer in the load order are silently dropped.
    //
    // ---------------------------------------------------------------------
    // Engine coupling: pushed out to the two call boundaries
    // ---------------------------------------------------------------------
    //
    // This class holds no engine types and calls no engine functions, so it
    // builds into NarrativeEngineCore and is unit-tested by
    // src/SenderCooldownTable.test.cpp with no Skyrim running. The two things
    // it genuinely needs from the game are supplied by the caller:
    //
    //   * The clock. `Stamp` and `IsOnCooldown` take `nowGameHours` rather
    //     than calling `EngineUtils::GetCurrentGameHours()` themselves.
    //     Reading `RE::Calendar` is main-thread work the beats already do, and
    //     making it a parameter turns "what time is it" from an ambient
    //     dependency into an ordinary argument a test can simply pass.
    //
    //   * The co-save. `Serialize` / `Deserialize` take an `ICosaveIO&`
    //     instead of an `SKSE::SerializationInterface*`. Production passes
    //     `SKSECosaveIO`; tests pass an in-memory fake and can reproduce a
    //     truncated record or a form whose plugin has been uninstalled.
    //
    // `FormID` here is `std::uint32_t`, which is exactly what `RE::FormID` is
    // an alias for, so call sites holding an `RE::FormID` pass it unchanged.
    class SenderCooldownTable
    {
    public:
        using FormID = std::uint32_t;

        SenderCooldownTable() = default;
        SenderCooldownTable(const SenderCooldownTable&) = delete;
        SenderCooldownTable& operator=(const SenderCooldownTable&) = delete;

        // Record `nowGameHours` as the last-consumed stamp for the
        // given sender. No-op when senderFormID == 0. Safe from any
        // thread; the caller's clock read happens outside the lock so
        // the stamp write is a straight assignment.
        void Stamp(FormID senderFormID, double nowGameHours);

        // Returns true if `senderFormID` was stamped within the last
        // `cooldownHours` in-game hours as of `nowGameHours`.
        // `cooldownHours <= 0` (cooldown disabled) and `senderFormID == 0`
        // both return false without touching storage.
        bool IsOnCooldown(FormID senderFormID, int cooldownHours, double nowGameHours) const;

        // Return the raw game-hours stamp for `senderFormID`, or
        // nullopt if this sender has never been stamped. Used by the
        // memory-watermark filtering path — semantics are "memories
        // recorded before this game-hours value should be excluded
        // from the sender's memory tail" (a hard filter, distinct
        // from IsOnCooldown's decaying-window semantics). Table is
        // reused for that role rather than duplicating storage.
        std::optional<double> GetStampGameHours(FormID senderFormID) const;

        void Clear();

        // Called with an ICosaveIO whose record the caller has already
        // opened. Writes the count-and-entries payload described above.
        void Serialize(ICosaveIO& io) const;

        // Reads the same payload from an already-open record. On any
        // short-read failure, clears the table and returns false so
        // the caller can log the error at its own site. Successful
        // load returns true.
        bool Deserialize(ICosaveIO& io);

    private:
        mutable std::mutex mutex_;
        std::unordered_map<FormID, double> stamps_;
    };
} // namespace NarrativeEngine
