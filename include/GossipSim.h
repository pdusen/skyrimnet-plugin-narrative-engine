#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <PluginThread.h>

#include <RE/Skyrim.h>

namespace SKSE
{
    class SerializationInterface;
}

// GossipSim — the rumor propagation simulation.
//
// A discrete-event model over GossipGraph, advanced by in-world time.
// Work is proportional to the number of TRANSMISSIONS rather than to the
// size of the population, so an idle world costs nothing: each carrier
// has exactly one scheduled event outstanding at a time, and a rumor
// with no live carriers has none.
//
// See docs/implementation/PHASE_13_GOSSIP_PROPAGATION.md for the model
// and tests/gossip-spread/PHASE_13_SIR_VALIDATION_LOG.md for the offline
// run that produced the tuning. Two properties of that model are load-bearing and easy to
// break by "simplifying":
//
//   * Contact is a FINITE PER-PERSON DAILY BUDGET divided among a
//     carrier's contacts by relative weight — never a per-pair rate. A
//     per-pair rate makes social activity scale with settlement size and
//     saturates the entire province within one game day.
//
//   * A carrier's telling quota is consumed by tellings that land on
//     someone who ALREADY KNOWS, not only by successful ones. That is
//     the saturation brake; without it the model does not terminate.
//
// ---------------------------------------------------------------------
// What a transmission writes
//
// Two real AddMemory calls, one for each party, composed by
// GossipContent from the rumor's generation-banded text plus
// relationship-aware framing. No LLM is involved anywhere in this
// module: the single call that produces the band text happens once, at
// seed time, before the rumor is ever handed here.
//
// Both memories are typed KNOWLEDGE and tagged "gossip", which is what
// keeps gossip's own output out of GossipHarvest's candidate set.
//
// ---------------------------------------------------------------------
// Threading
//
// Everything here runs on the plugin thread, driven by Tick. Game time
// is SAMPLED as a value each poll, never used as a timer. No engine
// mutation, and the only engine reads are the alive/disabled check on a
// prospective listener and the game-time sample — both safe off the main
// thread. The co-save callbacks run on SKSE's serialization thread and
// are mutex-guarded against the poll.
namespace NarrativeEngine::GossipSim
{
    // SKSE co-save record type. Frozen — changing it orphans every
    // previously-saved payload.
    inline constexpr std::uint32_t kRecordTypeId = 'NEGS';

    void Initialize();

    // kNewGame / kPostLoadGame. Refreshes the graph's relationship
    // layer and re-bases the game-time sample so a load does not look
    // like a colossal time jump.
    void OnSessionStart();

    // kPreLoadGame. Writes the live-rumor census into the gossip log
    // before GossipLog closes its file.
    void OnSessionEnd();

    // Plugin-thread poll on the Tick-driven accumulator. Samples game
    // time, then processes at most iGossipMaxEventsPerTick due events.
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);

    // Introduce a rumor originating with `originNpc`, sourced from
    // `sourceMemoryId`. Returns the rumor's id, or 0 if it could not be
    // seeded (graph not ready, origin not a participant, or the
    // live-rumor cap is full).
    // `bands` is the generation-banded text, produced by one LLM call at
    // seed time. Band selection at transmission time is by the receiving
    // carrier's generation.
    std::uint32_t SeedRumor(RE::FormID originNpc,
                            float notability,
                            std::int64_t sourceMemoryId,
                            std::vector<std::string> bands);

    struct Stats
    {
        std::size_t liveRumors = 0;
        std::size_t totalCarriers = 0;
        std::size_t queuedEvents = 0;
        std::size_t transmissionsThisSession = 0;
        std::size_t wastedThisSession = 0;
        // Conversation outcomes. transmissions + wasted + notCaught +
        // unavailable + capped accounts for every conversation drawn.
        std::size_t notCaughtThisSession = 0;
        std::size_t unavailableThisSession = 0;
        std::size_t cappedThisSession = 0;
        std::size_t memoriesWritten = 0;
        std::size_t memoryWriteFailures = 0;
    };
    Stats GetStats();

    // A read-only per-rumor snapshot for the dashboard. Flattened at call
    // time under the simulation mutex so the caller never holds a
    // reference into live state.
    struct RumorView
    {
        std::uint32_t id = 0;
        // Band 0 — the freshest telling. The dashboard shows this as the
        // rumor's identity; `bands` carries the rest for the expanded row.
        std::string text;
        std::vector<std::string> bands;

        // STALLED means every still-infectious carrier has run out of
        // people to tell: each of their named contacts already carries
        // the rumor. It is live but going nowhere.
        //
        // "Named" is the operative word. Every carrier also holds a
        // province-wide lottery ticket (the kProvincePeer sentinel, at
        // fGossipWeightProvince) that can reach any participant at all,
        // so no carrier is ever exhausted in the strictest sense. Judging
        // stall on that would mean nothing is ever stalled, which is
        // useless. A stalled rumor can therefore still jump — rarely —
        // and un-stall itself, which is correct behaviour rather than a
        // glitch in the readout.
        bool stalled = false;
        // False only in the window between the last carrier retiring and
        // the reap at the end of that same poll, so in practice the
        // dashboard never sees it. Carried anyway rather than asserted
        // away: the pairing of live+stalled is what makes the state
        // unambiguous if the reap is ever decoupled from the poll.
        bool live = true;

        float notability = 0.0f;
        double ageDays = 0.0;  // since seeding
        double idleDays = 0.0; // since the last successful telling

        std::size_t carriers = 0;       // everyone who has ever held it
        std::size_t activeCarriers = 0; // still infectious
        std::size_t settlements = 0;
        std::size_t holds = 0;
        std::uint32_t maxDepth = 0;
        std::size_t transmissions = 0;
        std::size_t wasted = 0;

        RE::FormID originNpc = 0;
        std::string originName;
        std::string originLocation;
        std::int64_t sourceMemoryId = 0;
    };

    // The share of `npc`'s named contact weight that currently resolves to
    // somebody able to hold a conversation, in [0, 1]. Returns 0 when they
    // have no named contacts at all.
    //
    // This IS the probability that one conversation drawn by `npc` reaches
    // a reachable listener, because peer selection is weighted by exactly
    // these rates — so it predicts directly how much of a carrier's quota
    // will be spent on people who are dead, disabled, or otherwise away.
    //
    // A weighted share rather than a count, because the two disagree badly.
    // Personal edges weigh 40 against a settlement neighbour's 1.0, so an
    // NPC with a handful of faction-mates in an unstarted quest can carry
    // most of their contact weight on people who do not exist yet while
    // still having a dozen perfectly available neighbours. Ancano seeded
    // five rumors in one session and every one reached nobody: 61% of
    // their conversations went to unavailable listeners, which a count of
    // available contacts would not have predicted.
    //
    // The province sentinel is excluded, as it is for the stall test: it
    // resolves to a random participant at transmission time and is a
    // lottery ticket rather than a contact.
    float AvailableContactShare(RE::FormID npc);

    // Every rumor still in the map, newest first. The map holds exactly
    // the rumors that have not been reaped, which is what the dashboard
    // wants: a rumor is listed until its last carrier retires.
    std::vector<RumorView> GetRumorViews();

    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
} // namespace NarrativeEngine::GossipSim
