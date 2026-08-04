#pragma once

// GossipSeeder — TEMPORARY. Milestone 1 validation only.
//
// Plants a stratified spread of stub rumors at session start so the
// gossip simulation has something to propagate before real seeding
// exists. It is not a feature and has no place in the shipped mod:
// delete this module wholesale when Milestone 2 wires seeding to
// NarrativeEngine's own event logs and memory store.
//
// Seeds are stratified rather than random, because the point is to
// cover the axes the offline validation identified as behaviourally
// distinct — settlement size and notability — rather than to sample the
// population uniformly. Uniform seeding would need far more trials to
// say anything about either.
//
//   4  large settlement, notability 0.90-1.00  should travel furthest
//   3  mid settlement (6-20),  notability 0.50-0.70  the common case
//   3  small settlement (<=5), notability 0.50-0.70  saturate-and-die
//   2  any settlement,         notability 0.20-0.30  should die fast
//
// The mix scales proportionally when iGossipStubSeedCount is not 12.
//
// Determinism matters here: the RNG comes from iGossipRandomSeed, so a
// run can be repeated exactly after a tuning change. A test that cannot
// be re-run identically makes a tuning comparison meaningless.
//
// Gated on bGossipSeedStubsOnLoad, which ships false.

#include <PluginThread.h>

namespace NarrativeEngine::GossipSeeder
{
    // kNewGame / kPostLoadGame, after GossipSim::OnSessionStart. No-op
    // unless both bGossipEnabled and bGossipSeedStubsOnLoad are set.
    // Also no-ops when the simulation already has live rumors restored
    // from the co-save, so reloading mid-run does not double-seed.
    void OnSessionStart();

    // Keeps planting during the run, one rumor every
    // iGossipStubSeedIntervalGameHours of in-world time up to
    // iGossipStubSeedMaxTotal for the session.
    //
    // Without this the harness is a one-shot: the initial batch all
    // burns out inside about a week of game time and the session goes
    // silent, which is exactly what happened on the second validation
    // run. It also fixes the sample-size problem — a dozen rumors
    // cannot be compared meaningfully against distributions whose reach
    // ranges from 2 to 100.
    //
    // Driven from Tick on the plugin thread, game-time paced.
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);
} // namespace NarrativeEngine::GossipSeeder
