#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <RE/T/TESForm.h>

namespace SKSE { class SerializationInterface; }

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
    // FormIDs pass through SKSE's ResolveFormID on load; entries whose
    // owner-mod is no longer in the load order are silently dropped.
    class SenderCooldownTable
    {
    public:
        SenderCooldownTable() = default;
        SenderCooldownTable(const SenderCooldownTable&) = delete;
        SenderCooldownTable& operator=(const SenderCooldownTable&) = delete;

        // Record now-game-hours as the last-consumed stamp for the
        // given sender. No-op when senderFormID == 0. Safe from any
        // thread; the calendar read runs outside the lock so the
        // stamp write is a straight assignment.
        void Stamp(RE::FormID senderFormID);

        // Returns true if `senderFormID` was stamped within the last
        // `cooldownHours` in-game hours. `cooldownHours <= 0`
        // (cooldown disabled) and `senderFormID == 0` both return
        // false without touching storage.
        bool IsOnCooldown(RE::FormID senderFormID, int cooldownHours) const;

        void Clear();

        // Called with an already-open SKSE cosave record. Writes the
        // count-and-entries payload described above.
        void Serialize(SKSE::SerializationInterface* intfc) const;

        // Reads the same payload from an already-open record. On any
        // short-read failure, clears the table and returns false so
        // the caller can log the error at its own site. Successful
        // load returns true.
        bool Deserialize(SKSE::SerializationInterface* intfc);

    private:
        mutable std::mutex                     mutex_;
        std::unordered_map<RE::FormID, double> stamps_;
    };
}
