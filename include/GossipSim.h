#pragma once

#include <cstdint>

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
// and PHASE_13_VALIDATION_LOG.md for the offline run that produced the
// tuning. Two properties of that model are load-bearing and easy to
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
// Milestone 1 scope
//
// Memories written on each transmission are STUBS: real AddMemory calls
// carrying placeholder text and a notability-derived importance, tagged
// so they can be found and purged. No LLM is involved anywhere in this
// module. Real content generation is Milestone 2.
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

    // Introduce a rumor originating with `originNpc`. `slice` is a
    // free-form label recorded in the log so a stratified seeding run
    // can be read back against its seed conditions. Returns the rumor's
    // id, or 0 if it could not be seeded (graph not ready, origin not a
    // participant, or the live-rumor cap is full).
    std::uint32_t SeedRumor(RE::FormID originNpc, float notability, std::string_view slice);

    struct Stats
    {
        std::size_t liveRumors = 0;
        std::size_t totalCarriers = 0;
        std::size_t queuedEvents = 0;
        std::size_t transmissionsThisSession = 0;
        std::size_t wastedThisSession = 0;
        std::size_t memoriesWritten = 0;
        std::size_t memoryWriteFailures = 0;
    };
    Stats GetStats();

    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
} // namespace NarrativeEngine::GossipSim
