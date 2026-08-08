#pragma once

#include <cstdint>
#include <vector>

namespace SKSE
{
    class SerializationInterface;
}

// GossipClaims — the ledger of memories already turned into rumors.
//
// One record per claimed memory: its id and the game day the claim expires.
// A memory that has been a rumor can never be one again while its claim
// stands, which is what stops the same story circulating twice.
//
// ---------------------------------------------------------------------
// The invariant
//
// `fGossipClaimExpiryDays` MUST EXCEED `fGossipHarvestWindowDays`.
//
// A memory claimed the instant it was created has its claim expire at the
// former, but stops being harvestable at the latter — so the claim always
// outlives its memory's eligibility, and expiry can never hand a memory
// back while it is still a candidate. Invert the two and a memory claimed
// early becomes re-harvestable partway through its life.
//
// Settings::Load checks this and logs an error rather than clamping: a
// tuning pass that adjusts one number without the other should be visible.
//
// ---------------------------------------------------------------------
// Why this can be small
//
// Because a claim outlives eligibility, a memory can be claimed at most
// once and can never re-enter the harvest window afterwards. That also
// resolves the immunity question: under SIR immunity is per-rumor and
// lives in that rumor's carrier map, so re-seeding the same story would
// make everyone who heard it susceptible again. Preventing re-seeding
// outright means that never arises — and it is why burned-out rumors can
// be reaped whole rather than retaining their carrier sets.
//
// One entry per rumor seeded, held for the expiry window. Even at the
// Milestone 1 harness's deliberately aggressive four rumors per game day
// that is a few hundred entries.
//
// ---------------------------------------------------------------------
// Threading
//
// Plugin thread, mutex-guarded internally. The co-save callbacks run on
// SKSE's serialization thread and take the same lock.
namespace NarrativeEngine::GossipClaims
{
    // SKSE co-save record type. Frozen — changing it orphans saved
    // payloads.
    //
    // One record holds everything: each memory claim is followed by the
    // ids of the events it claimed. The events carry neither their own
    // expiry nor an owner backref on disk, because both are implied by the
    // claim they sit under — which also makes a dangling event claim
    // unrepresentable rather than merely unlikely.
    inline constexpr std::uint32_t kRecordTypeId = 'NEGC';

    void Initialize();

    // True while `memoryId` is claimed. The harvester's qualification gate.
    bool IsClaimed(std::int64_t memoryId);

    // True while ANY id in `eventIds` is claimed.
    //
    // This is what stops one event becoming several rumors. SkyrimNet
    // writes a separate memory to every actor who was present, each with
    // its own id but with overlapping `related_event_ids`; per-memory
    // dedup alone happily seeds all of them. On the first in-game run six
    // of ten rumors were duplicates of just two events — three memories of
    // one confrontation had byte-identical event sets.
    bool AreEventsClaimed(const std::vector<std::int64_t>& eventIds);

    // Claim `memoryId`, and separately claim every id in `eventIds`, until
    // `nowGameDay + fGossipClaimExpiryDays`.
    //
    // Re-claiming an already-claimed id refreshes nothing and is a no-op —
    // the original expiry stands, so a claim cannot be extended
    // indefinitely by repeated attempts. Event claims record the memory
    // that took them so Release can find them again.
    void Claim(std::int64_t memoryId, const std::vector<std::int64_t>& eventIds, double nowGameDay);

    // Hand a memory back immediately, along with every event claim it
    // took.
    //
    // Needed because a rumor is claimed BEFORE its content is generated:
    // if generation fails, or the model judges the memory not worth
    // gossiping about, the claim must be released or a transient error
    // would permanently burn a memory that never produced anything. The
    // events must come back too, or a failed generation would keep every
    // other witness's account of the same event locked out for the whole
    // expiry window.
    void Release(std::int64_t memoryId);

    // Drop expired claims. Called from the simulation poll on sampled game
    // time rather than on rumor activity, so a quiet stretch with no live
    // rumors still expires claims. Returns how many were dropped.
    std::size_t Sweep(double nowGameDay);

    std::size_t Count();
    std::size_t EventCount();

    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
} // namespace NarrativeEngine::GossipClaims
