#pragma once

#include <PluginThread.h>

// GossipTick — the scheduler, and the single unit of work it schedules.
//
// Everything gossip does in one beat of in-world time is ONE job, run
// start to finish on GossipDispatch's worker:
//
//   1. harvest    rank actors, fetch memories, qualify, build the pool
//   2. generate   evaluate candidates and compose the winner (LLM)
//   3. simulate   drain every carrier-step due by the tick's horizon
//   4. prune      reap dead rumors, expire claims
//   5. flush      write the trace
//   6. publish    swap the dashboard/co-save snapshot
//
// ---------------------------------------------------------------------
// Scheduled, stamped, never coalesced
//
// A tick is due every fGossipHarvestIntervalGameHours of in-world time.
// Passing 24 hours with `T` crosses two of those boundaries and TWO
// ticks run — not one that happens to have twice as much to do. The
// plugin-thread check below does not ask "is gossip busy?"; it asks how
// many scheduled times have gone by that it has not enqueued yet, and
// enqueues one job per boundary. The queue is the backlog, and because
// GossipDispatch is serial and ordered they run in schedule order with
// the earlier tick's rumors already seeded before the later one looks at
// the world.
//
// Each job carries the game time it was SUPPOSED to fire at, and reads
// the world as of that moment — memories stamped later are ignored, the
// simulation clock is set to it rather than sampled, and claims are
// dated by it. That is what decouples when a tick runs from what it
// processes, and it is why the queue can back up behind a slow LLM call
// without the simulation drifting.
//
// One place it cannot be exact: GetActorEngagement reports engagement as
// of now and offers no time filter, so WHICH actors get examined is
// unavoidably current even when the memories examined are not. The harm
// is bounded — it can mis-prioritise, but every memory it then reads is
// still filtered, so no post-horizon content can reach a rumor.
//
// See docs/implementation/PHASE_13_MILESTONE_3.md.
namespace NarrativeEngine::GossipTick
{
    // Ticks that may be queued or running before the scheduler stops
    // enqueuing and starts advancing the schedule without work.
    //
    // Inherits the old harvest accumulator's kMaxOwedSweeps: there is no
    // value in harvesting the same memory corpus six times in a row, and
    // a console time jump must not be able to queue a year of simulation.
    inline constexpr std::size_t kMaxOutstandingTicks = 4;

    void Initialize();

    // kNewGame / kPostLoadGame. Re-bases the schedule onto the current
    // game clock so a load does not read as a colossal backlog.
    void OnSessionStart();

    // The plugin-thread cadence check, and the ONLY gossip work that
    // still happens there. Accumulates unpaused real seconds against
    // iGossipTickIntervalSeconds, samples the game clock, and enqueues
    // one stamped job per crossed interval. Microseconds, no locks on
    // gossip state, no possibility of blocking.
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);
} // namespace NarrativeEngine::GossipTick
