#pragma once

#include <cstdint>
#include <string_view>

#include <GossipGraph.h>
#include <PluginThread.h>

#include <RE/Skyrim.h>

// GossipLog — a dedicated trace of the rumor mill, and nothing else.
//
// Writes to Data/../SKSE/NarrativeEngine_Gossip.log, session-scoped and
// rotated five deep, following the EventHistoryWriter pattern. Unlike
// EventHistoryWriter it owns its own spdlog sink rather than borrowing
// the default one, so nothing it writes reaches NarrativeEngine.log and
// nothing from elsewhere reaches it.
//
// It is gated on bGossipLogEnabled ALONE — deliberately not on
// bDebugMode. The whole point is to be able to run a long validation
// session with a quiet main log and a complete gossip trace.
//
// The format is designed to be analysable rather than merely verbose.
// Every TELL line names the CHANNEL the transmission travelled by, and
// hold crossings are flagged inline, because channel attribution is what
// established that organisations rather than families move rumors
// between holds. A run can be assessed from the BURNOUT lines alone.
//
// Threading: every emitter is callable from the plugin thread and takes
// no engine locks. Names come from GossipGraph's cached strings, never
// from a live RE:: pointer. Internally mutex-guarded, so the ordering of
// concurrent emitters is consistent even though only one worker drives
// the simulation today.
namespace NarrativeEngine::GossipLog
{
    // Registers the module and reads settings. Does NOT touch the
    // filesystem — the file lifecycle is scoped to save-game sessions.
    void Initialize();

    // kNewGame / kPostLoadGame. Rotates the previous five files and
    // opens a fresh one. No-op when logging is disabled.
    void OnSessionStart();

    // kPreLoadGame. Writes the closing census, flushes, closes.
    void OnSessionEnd();

    // Tick-driven flush, so a crash loses at most one interval of lines
    // rather than the whole session's tail.
    // NOTE ON BUFFERING. Every line is flushed as it is written, and
    // that is deliberate rather than careless.
    //
    // A gossip tick blocks for two LLM round trips, so anything less than
    // per-line flushing means the trace goes silent for ten seconds at a
    // time and then arrives in a burst — and worse, the stream's own
    // buffer fills wherever it fills, so a tail of the file sits on a
    // HALF-WRITTEN LINE while it waits. That is exactly what an earlier
    // revision did: it flushed once at the end of each tick, and the file
    // was unreadable while anything interesting was happening.
    //
    // The cost is a few hundred flushes per tick on a thread that has
    // nothing else to do, against two multi-second network calls. The
    // whole value of a trace is being readable while the thing it traces
    // is running.
    //
    // GossipLog also keeps its mutex, unlike the rest of gossip. It is a
    // file writer rather than simulation state, and its session-boundary
    // writes still come from the main thread; a mutex on a log write is
    // not what Milestone 3 is trying to remove.

    // True when a file is open and lines will actually land.
    bool IsActive();

    // --- emitters -----------------------------------------------------

    // A rumor enters the world, naming the memory it came from.
    void Seed(std::uint32_t rumorId,
              float notability,
              RE::FormID originNpc,
              RE::FormID settlement,
              std::int64_t sourceMemoryId);

    // One successful transmission. `via` is the specific channel that best
    // explains the contact; `tier` is the proximity tier the pair share.
    // Both are logged because they diverge constantly -- most vanilla
    // relationship edges are between housemates, so collapsing them made
    // household traffic look like a sixth of what it actually is.
    void Tell(std::uint32_t rumorId,
              std::uint32_t generation,
              float notability,
              RE::FormID from,
              RE::FormID to,
              GossipGraph::Channel via,
              GossipGraph::Channel tier,
              RE::FormID viaFaction,
              RE::FormID location,
              RE::FormID fromHold,
              RE::FormID toHold);

    // A conversation that landed on somebody who already knows — either
    // still infectious or recovered and immune. These are the wasted
    // opportunities that bring an outbreak to a halt, so a run where
    // almost nothing is wasted means nothing is saturating anywhere.
    // `remaining` is how many conversations are left in this step.
    void Wasted(std::uint32_t rumorId, RE::FormID from, RE::FormID to, int remaining);

    // A carrier stops transmitting and becomes permanently immune.
    // `reason` is one of "recovered" (the infectious period elapsed —
    // the normal case), "age", "dead", or "no-contacts".
    void Retire(std::uint32_t rumorId, RE::FormID npc, std::string_view reason);

    // A rumor's last carrier retired. Carries the whole per-rumor
    // summary so a validation run can be assessed from these lines
    // alone.
    struct BurnoutStats
    {
        std::size_t reach = 0;
        std::uint32_t depth = 0;
        std::size_t holds = 0;
        std::size_t settlements = 0;
        double days = 0.0;
        std::size_t transmissions = 0;
        std::size_t wasted = 0;
        // Every conversation the rumor's carriers drew, and where each one
        // went. The five outcomes sum to `conversations`, so a rumor that
        // told nobody says whether it never spoke or simply never landed.
        std::size_t conversations = 0;
        std::size_t notCaught = 0;
        std::size_t unavailable = 0;
        std::size_t capped = 0;
    };
    void Burnout(std::uint32_t rumorId, const BurnoutStats& stats);

    // One harvest sweep. Every count is for THIS sweep alone — a
    // cumulative figure alongside a per-sweep one cannot be read as a
    // rate, and the rate is the whole question the sweep log answers.
    struct HarvestStats
    {
        // Which bucket this sweep drew, and how many participants were
        // in it. Replaces the old sampled/seen pair: there is no ranking
        // to take a sample of any more, so "25 of 144" described a
        // relationship that no longer exists.
        std::uint32_t bucket = 0;
        std::size_t bucketCount = 0;
        std::size_t bucketPopulation = 0;
        std::size_t memoriesExamined = 0;
        std::size_t candidates = 0;
        std::size_t sentForGeneration = 0;
        std::size_t rejectedTooOld = 0;
        std::size_t rejectedLowImportance = 0;
        std::size_t rejectedClaimed = 0;
        std::size_t rejectedDiary = 0;
        std::size_t rejectedNoContent = 0;
        std::size_t rejectedNoGameTime = 0;
        std::size_t rejectedOwnOutput = 0;
        std::size_t rejectedSameEvent = 0;
        std::size_t rejectedIsolated = 0;
    };
    void Harvest(const HarvestStats& stats);

    // One memory's fate in a sweep. `verdict` is "candidate" or the
    // rejection reason ("too-old", "low-importance", "wrong-type",
    // "claimed"). Emitted per examined memory, which is verbose by
    // design: a sweep that finds nothing is the interesting case, and
    // "found nothing" is only diagnosable if the near-misses are named.
    void Memory(std::int64_t memoryId, RE::FormID owner, float importance, std::string_view verdict);

    // A memory enters or leaves the ledger. `action` is "claimed",
    // "released" or "expired".
    void Claim(std::int64_t memoryId, std::string_view action, std::size_t outstanding);

    // Free-form note line, for anything that does not fit the shapes
    // above: queue-depth warnings, catch-up drains, content-generation
    // failures, and the end-of-session census.
    //
    // The census is written by GossipSim rather than here, so that this
    // module never has to know the simulation's types. Plugin.cpp orders
    // GossipSim::OnSessionEnd before GossipLog::OnSessionEnd for exactly
    // that reason.
    void Note(std::string_view text);
} // namespace NarrativeEngine::GossipLog
