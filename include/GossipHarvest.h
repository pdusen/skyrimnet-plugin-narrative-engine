#pragma once

#include <cstddef>

#include <PluginThread.h>

// GossipHarvest — turns SkyrimNet memories into rumors.
//
// ---------------------------------------------------------------------
// Why this is two calls
//
// There is no global memory query. `PublicGetMemoriesForActor` is strictly
// per-actor, and the only endpoint taking `formId = 0` for "all actors" is
// `PublicGetDiaryEntries`, which returns diary entries — a different thing,
// and one the letter and visit composers deliberately filter out.
//
// So a province-wide sweep is assembled:
//
//   Stage 1  GetActorEngagement(0, ...) returns EVERY actor with any
//            activity, each row carrying recentMemoryImportanceMedium.
//            With the medium window set to the harvest window that is
//            exactly the ranking signal wanted: "this actor has important
//            things that happened to them lately."
//
//   Stage 2  GetMemoriesForActor on the top N of that ranking, then
//            qualify per-memory.
//
// One cheap global call decides where to spend the expensive per-actor
// ones. The alternative is ~880 per-actor calls per sweep, almost all
// returning nothing of interest.
//
// ---------------------------------------------------------------------
// The feedback loop
//
// Gossip writes memories. If those could seed rumors, a rumor reaching
// twenty people would write forty memories, which would become forty
// rumors, without bound.
//
// A returned memory row carries, among others: id, content, type, tags,
// importance_score, decayed_importance, age_hours, game_time,
// creation_time, emotion, location, related_actors.
//
// `tags` IS present, so the ["gossip"] tag written at AddMemory time is
// filtered on directly, and that is the whole feedback-loop guard.
//
// There is deliberately NO memory-type allowlist. One existed while `tags`
// was believed absent, restricting sources to EXPERIENCE/TRAUMA/JOY so that
// gossip's own KNOWLEDGE output could not re-seed. Once the tag became
// available that rule was doing nothing but discarding material: on a real
// save KNOWLEDGE is the largest bucket AND the highest-scoring one, JOY does
// not occur at all, and the best gossip on the save — a public affair, a
// confrontation in a Dwemer ruin — was being thrown away by it.
//
// ---------------------------------------------------------------------
// Field names: trust the endpoint, not its documentation
//
// PublicAPI.h documents this response as {id, text, importance, timestamp,
// type}. THREE OF THOSE FIVE ARE WRONG. The real names are `content`,
// `importance_score` and `age_hours`, corroborated by the `memories` table
// in SkyrimNet's sql/migrations/0003_vector_memory_system.sql and by
// SenderCandidatePool, which has been reading this same endpoint correctly
// since the letter beat shipped.
//
// `age_hours` is not merely `timestamp` renamed, and it is NOT game time:
// it is REAL-WORLD elapsed time since the row was written, derived from
// `creation_time`. Use `game_time` for anything in-world. See the finding
// doc below — this one cost a test run on its own.
//
// Every one of these fails soft. nlohmann's value() returns the default for
// a missing key, so a wrong name reads as 0.0 or "" and the row is rejected
// by a rule that looks like it is working. The first in-game run examined
// 162 memories and rejected all 162 as low-importance, every one of them
// reporting imp=0.00.
//
// Full write-up, including why only the first wrong field ever surfaces:
// docs/engine-findings/skyrimnet-memory-json-field-names.md
//
// ---------------------------------------------------------------------
// Threading
//
// Plugin thread, driven by Tick on the standard accumulator, paced on
// sampled game time. SkyrimNet's data queries are documented thread-safe;
// no engine mutation happens here.
namespace NarrativeEngine::GossipHarvest
{
    void Initialize();

    // kNewGame / kPostLoadGame. Re-bases the game-time pacing sample.
    void OnSessionStart();

    // Tick-driven. Runs a sweep every fGossipHarvestIntervalGameHours of
    // in-world time; no-op otherwise.
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);

    // Session totals, reset on OnSessionStart. Per-sweep numbers go to
    // the gossip trace as HARVEST lines — the two are kept apart on
    // purpose, since a cumulative count and a per-sweep count printed
    // side by side cannot be read as a rate.
    struct Stats
    {
        std::size_t sweeps = 0;
        std::size_t actorsRanked = 0;
        std::size_t memoriesExamined = 0;
        // Memories handed to GossipContent, which is not the same as
        // rumors that exist: generation can still fail or be refused,
        // and the claim is released when it is.
        std::size_t sentForGeneration = 0;
        std::size_t rejectedTooOld = 0;
        std::size_t rejectedLowImportance = 0;
        std::size_t rejectedNotParticipant = 0;
        std::size_t rejectedClaimed = 0;
        std::size_t rejectedDiary = 0;
        std::size_t rejectedNoContent = 0;
        std::size_t rejectedNoGameTime = 0;
        std::size_t rejectedOwnOutput = 0;
        // Another witness's account of the same happening already seeded.
        std::size_t rejectedSameEvent = 0;
        // The origin's contacts are mostly unreachable, so it could not
        // have spread the rumor anywhere.
        std::size_t rejectedIsolated = 0;
    };
    Stats GetStats();
} // namespace NarrativeEngine::GossipHarvest
