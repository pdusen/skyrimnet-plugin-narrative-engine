#pragma once

#include <cstdint>
#include <deque>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <RE/Skyrim.h>

// GossipState — every mutable field the gossip simulation owns, in one
// copyable struct.
//
// These types lived as file-static globals split across GossipSim.cpp
// and GossipClaims.cpp. Collecting them buys three things that were not
// otherwise available:
//
//   1. ONE OWNER. The gossip worker holds the live instance and nothing
//      else touches it, which is what lets those modules carry no mutex.
//
//   2. ONE SNAPSHOT. A `shared_ptr<const GossipState>` published at the
//      end of a tick is a consistent image of the whole world at one
//      instant, which the dashboard and the co-save both read. Rumors
//      and claims can never be observed — or saved — from different
//      moments, which they could when each module published its own.
//
//   3. ONE SAVE FORMAT. The co-save records are written from the
//      snapshot rather than from a parallel description of the same
//      state that can drift away from it.
//
// Everything here must stay CHEAPLY COPYABLE: no std::function, no
// owning pointers, no self-referential handles. A tick copies this
// wholesale to publish, and the copy has to be a memcpy-ish walk of
// trivial elements rather than a graph traversal.
//
// What is deliberately NOT here:
//
//   * The contact cache. Derived from GossipGraph, rebuilt on demand,
//     and hundreds of times larger than everything else combined —
//     881 participants times ~100 contacts each. Snapshotting it would
//     make the copy the dominant cost of a tick to preserve something
//     that can simply be recomputed.
//   * The RNG. A generator, not world state. Not saved today either.
//   * Scheduler pacing. `secondsSinceTick` and the owed-sweep
//     accumulator belong to whoever decides when a tick runs, not to
//     the world the tick simulates.
//
// See docs/implementation/PHASE_13_MILESTONE_3.md.
namespace NarrativeEngine::GossipState_
{
    // Carrier state under SIR. A carrier is Infectious from
    // `heardOnGameDay` until `infectiousUntilGameDay`, then Recovered —
    // permanently immune and never re-infectable.
    //
    // Note what is NOT here: no per-carrier notability, no telling quota,
    // no household-saturation flag. Transmissibility is constant and lives
    // on the rumor; nothing about a carrier depletes. Three earlier models
    // failed by making spread a function of how far a rumor had already
    // travelled, and every one of those fields was part of that mistake.
    struct Carrier
    {
        std::uint32_t generation = 0;
        RE::FormID toldBy = 0;
        double heardOnGameDay = 0.0;
        double infectiousUntilGameDay = 0.0;
        double nextStepGameDay = 0.0;
        bool recovered = false;
    };

    struct Rumor
    {
        std::uint32_t id = 0;
        RE::FormID originNpc = 0;
        RE::FormID originSettlement = 0;
        double seedGameDay = 0.0;
        // Constant for the rumor's whole life. Per-conversation
        // transmission probability is `notability * transmissionScale`.
        float notability = 1.0f;
        // Provenance. Without a recorded source, "no memory is ever
        // used twice" cannot be verified from the trace — a claimed
        // memory and the rumor it produced would only be correlated
        // by timing.
        std::int64_t sourceMemoryId = 0;
        RE::FormID sourceActor = 0;
        // Generation-banded text, produced at seed time. Selected by the
        // receiving carrier's generation.
        std::vector<std::string> bands;
        // Every NPC that has EVER carried this rumor. Membership here is
        // what makes someone immune, so it must never be pruned while the
        // rumor is live — a removed entry would be re-infectable and the
        // outbreak would never terminate.
        std::unordered_map<RE::FormID, Carrier> carriers;
        std::uint32_t maxDepth = 0;
        std::size_t transmissions = 0;
        std::size_t wasted = 0;
        // Conversation accounting. `conversations` is every one drawn;
        // the outcome buckets below sum to it exactly, so a rumor that
        // reached nobody can be told apart from one that never opened its
        // mouth. Without this, a conversation that happened and simply
        // failed the transmission roll was counted nowhere, and two dead
        // rumors could not be explained at all.
        std::size_t conversations = 0;
        std::size_t notCaught = 0;   // spoke, but it did not take
        std::size_t unavailable = 0; // listener down, gone, or unreachable
        std::size_t capped = 0;      // carrier cap already reached
        double lastActivityGameDay = 0.0;
        bool live = true;
    };

    struct QueueEntry
    {
        double dueGameDay = 0.0;
        std::uint32_t rumorId = 0;
        RE::FormID carrier = 0;

        // std::priority_queue is a max-heap; invert so the earliest
        // due event pops first.
        bool operator<(const QueueEntry& other) const
        {
            return dueGameDay > other.dueGameDay;
        }
    };

    // std::priority_queue hides its container as a protected member,
    // which makes "how many of these are due right now?" unanswerable
    // from outside. That question has to be answerable: the drain report
    // used to print the whole queue depth and call it the due count,
    // which reads as an unbounded backlog when it is nothing of the sort.
    //
    // Exposing `c` rather than switching to a hand-rolled vector heap
    // keeps the ordering semantics exactly as they were. The scan is a
    // linear pass over a container the size of the live-carrier set —
    // tens of entries — and only runs when the drain stopped early.
    struct EventQueue : std::priority_queue<QueueEntry>
    {
        using std::priority_queue<QueueEntry>::c;
    };

    // One claimed event. Keyed by event id in the map, so the harvester's
    // check is one hash probe per related event rather than a walk over
    // other memories' event lists.
    struct EventClaim
    {
        double expiresOnGameDay = 0.0;
        // Which memory took it. Release works from a memory id alone, so
        // the ownership has to live here rather than the caller having to
        // re-supply the event list it claimed with.
        std::int64_t claimedByMemoryId = 0;
    };

    // Session totals for the harvest side. Here rather than in
    // GossipHarvest's own global so that the dashboard reads them from
    // the same image as the rumors they produced — a sweep count and a
    // rumor count caught at different instants cannot be reconciled by
    // whoever is looking at them.
    struct HarvestCounters
    {
        std::size_t sweeps = 0;
        // Participants queried this session. Was a count of the ranked
        // sample; now the sum of the drawn buckets' populations, which is
        // the same question asked of a selection that no longer ranks.
        std::size_t actorsExamined = 0;
        std::size_t memoriesExamined = 0;
        std::size_t sentForGeneration = 0;
        std::size_t rejectedUnfiltered = 0;
        std::size_t rejectedClaimed = 0;
        std::size_t rejectedDiary = 0;
        std::size_t rejectedNoContent = 0;
        std::size_t rejectedSameEvent = 0;
        std::size_t rejectedIsolated = 0;
    };

    // Session totals. Reset at OnSessionStart, not persisted — they
    // describe this play session rather than the world.
    struct SessionCounters
    {
        std::size_t transmissions = 0;
        std::size_t wasted = 0;
        std::size_t notCaught = 0;
        std::size_t unavailable = 0;
        std::size_t capped = 0;
        std::size_t memoriesWritten = 0;
        std::size_t memoryWriteFailures = 0;
    };
} // namespace NarrativeEngine::GossipState_

namespace NarrativeEngine
{
    struct GossipState
    {
        // --- simulation ------------------------------------------------
        std::unordered_map<std::uint32_t, GossipState_::Rumor> rumors;
        GossipState_::EventQueue queue;
        std::uint32_t nextRumorId = 1;
        // The simulation's own clock. Under Milestone 3 this is SET to the
        // scheduled time of the tick being executed rather than sampled
        // from the game clock, which is what lets a late tick still be a
        // correct tick.
        double simGameDay = 0.0;
        // Last sampled game day, kept only so a session start can re-base
        // without reading a multi-year jump on the first tick.
        double lastGameDaySample = -1.0;
        GossipState_::SessionCounters counters;
        GossipState_::HarvestCounters harvest;

        // --- harvest bucket selection ----------------------------------
        //
        // Most-recent-last. Bounded at the draw to bucketCount-1 entries,
        // so this never grows: it is a window, not a log.
        //
        // Here rather than beside the scheduler because it must be saved
        // from the SAME instant as the rumors it produced. Split them and
        // a reload can restore a history whose exclusions describe draws
        // that seeded rumors the save does not contain.
        std::deque<std::uint32_t> bucketHistory;
        // How many buckets `bucketHistory` was recorded against. Change
        // iGossipHarvestBuckets mid-playthrough and every participant is
        // reassigned, so the remembered indices stop denoting the same
        // people; the load compares this and discards rather than
        // excluding an arbitrary set.
        std::uint32_t bucketCount = 0;

        // --- claim ledger ----------------------------------------------
        //
        // Here rather than beside the simulation because the two must be
        // saved from the SAME instant. Split them and a reload can restore
        // a rumor whose source memory is no longer claimed, leaving that
        // memory free to seed a second rumor about a happening already
        // going round.
        //
        // memoryId -> game day the claim expires.
        std::unordered_map<std::int64_t, double> claims;
        std::unordered_map<std::int64_t, GossipState_::EventClaim> eventClaims;
    };
} // namespace NarrativeEngine
