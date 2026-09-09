#pragma once

#include <GossipGraph.h>
#include <GossipSim.h>
#include <GossipState.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Stand-ins for the five gossip modules a gossip tick drives, plus the record
// of what it drove them with.
//
// These live beside the harness rather than in one test file because two test
// files need them: GossipTick's own, which drives a tick and checks the order
// of the steps, and Tick's, which only needs to know that the scheduler was
// reached at all. Defined twice they would be a duplicate symbol; defined in
// one of the two they would be a dependency between test files.
//
// They are spies as much as stand-ins. The ORDER a tick calls these in is
// itself the contract — the horizon is stamped before the harvest so a rumor
// seeded during the tick is not dated a whole interval in the past, and the
// snapshot is published once at the end so no reader ever sees a half-advanced
// simulation.
namespace NarrativeEngine::Testing
{
    using NarrativeEngine::GossipState;

    struct GossipSpyState
    {
        std::mutex mutex;

        // Every step a tick took, in order.
        std::vector<std::string> calls;
        // The game day each tick was stamped for.
        std::vector<double> stampedHorizons;
        // Display names the gossip trace renders. Filled in by a test so a
        // line can be checked for the name a reader would see.
        std::map<std::uint32_t, std::string> npcNames;
        std::map<std::uint32_t, std::string> locationNames;

        // How many times the scheduler asked whether the graph was ready. The
        // cheapest evidence that anything reached GossipTick::Poll at all,
        // which is what Tick's own tests need.
        int graphReadyQueries = 0;

        bool graphReady = true;
        bool sweepSucceeds = true;
        double lastSimulatedGameDay = -1.0;
        // Which step should observe a cancellation, so each of a tick's three
        // checkpoints can be reached in turn.
        std::string cancelAfter;

        // Who is in the graph. A rumor is only ever about participants, and
        // what the content layer wants from one is its name, where it lives
        // and who its people are — so the harness holds whole participants
        // rather than answering field by field.
        std::map<std::uint32_t, NarrativeEngine::GossipGraph::Participant> participants;

        // Rumors already circulating, which the evaluation is shown so it can
        // recognise one it has already heard.
        std::vector<NarrativeEngine::GossipSim::RumorView> circulating;

        // Rumors seeded, in order, with the banded text each was seeded with.
        struct Seeded
        {
            std::uint32_t originNpc = 0;
            float notability = 0.0f;
            std::int64_t sourceMemoryId = 0;
            std::vector<std::string> bands;
        };
        std::vector<Seeded> seeded;

        // What the next seed answers with. Zero is the simulation refusing —
        // the graph is not ready, the origin is not a participant, or the live
        // cap is full — and the content layer has to release its claim.
        std::uint32_t nextRumorID = 1;

        void Record(std::string step);
        std::vector<std::string> Calls();
        std::size_t CountOf(std::string_view step);
        void Reset();
    };

    GossipSpyState& GossipSpies();

    // The simulation's live and staged state. Real GossipState objects the
    // harness owns, because GossipState is a plain struct and the modules that
    // read it walk it field by field — a genuine one is simpler than a stand-in
    // and exactly faithful. Cleared by ResetGossipState.
    GossipState& LiveGossipState();
    GossipState& StagedGossipState();
    void ResetGossipState();

    // Every line the real gossip trace has written this session, read back off
    // disk. GossipLog is compiled for real into the test executable and writes
    // under EngineMock's log directory, so this is exactly the file a player
    // would attach to a bug report.
    std::vector<std::string> GossipTraceLines();

    // Removes the trace files so a case starts from an empty one. Closes the
    // current session first, because the stream holds the file open.
    void ClearGossipTrace();
} // namespace NarrativeEngine::Testing
