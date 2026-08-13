#pragma once

#include <cstddef>

#include <GossipDispatch.h>
#include <GossipState.h>
#include <GossipThread.h>
#include <PluginThread.h>

// GossipHarvest — turns SkyrimNet memories into rumors.
//
// ---------------------------------------------------------------------
// Who gets examined, and why it is not everyone
//
// There is no global memory query. `PublicQueryMemoriesForActor` is strictly
// per-actor, and the only endpoint taking `formId = 0` for "all actors" is
// `PublicGetDiaryEntries`, which returns diary entries — a different thing,
// and one the letter and visit composers deliberately filter out. So a
// province-wide sweep would mean ~880 calls into SkyrimNet's memory
// database, every sweep, most of them returning nothing of interest.
//
// The answer is NOT to pick the most interesting people. That was the
// first design: rank every active actor by `GetActorEngagement` and query
// the top 25. But engagement is a measure of interaction with THE PLAYER,
// so the ranking could only ever surface people the player had just been
// near — the mill reported the player's own itinerary back at them, and
// the other ~840 participants were structurally incapable of ever seeding
// anything. Ten rumors from six people, all inside one questline.
//
// The answer is to split the population instead. Every participant is
// assigned to one of `iGossipHarvestBuckets` buckets by a hash of their
// base FormID, fixed for the life of the save; a sweep draws one bucket
// and examines EVERY participant in it. Ten buckets turns one impossible
// sweep into ten tractable ones and still reaches everybody, just not at
// once. Bucket count is the cost lever, traded against how long a given
// NPC waits for a turn.
//
// Nothing in selection consults the player any more. A farmer in
// Rorikstead with a notable memory has the same prospect of seeding as an
// Arch-Mage the player talks to daily, which is the whole point.
//
// A bucket whose members hold nothing qualifying produces no rumor, and
// the sweep does NOT scan on to a fuller one. Early in a playthrough that
// would amplify the handful of people who have memories at all across the
// entire province — the concentration this design exists to remove,
// reintroduced where it does the most damage.
//
// See docs/implementation/PHASE_13_MILESTONE_4.md.
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
// The guard is `excludeTags` on the query itself (SkyrimNet API v10+), so
// gossip's own rows never leave the database. That matters more than it
// sounds: filtering them out on THIS side could only ever filter what
// survived SkyrimNet's truncation, and our writebacks are always the
// newest rows, so under the old endpoint's recency ranking they owned the
// entire result. GossipHarvest re-checks the tag on every row it receives,
// but purely as an alarm — a row that trips it means the server-side
// filter did not hold.
//
// The tag is `ne_gossip`, not `gossip`, because SkyrimNet's own memory
// tagger uses `gossip` as a TOPICAL label for memories that are merely
// about gossiping. A direct read of its database found 56 such rows in
// 1602 — real memories this guard was silently discarding, and precisely
// the social-drama material most worth gossiping about. A tag no one else
// writes cannot collide.
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
// type}, and the v10 query endpoint returns the same rows. THREE OF THOSE
// FIVE ARE WRONG. The real names are `content`,
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
    // The tag NarrativeEngine stamps on every memory it writes, and the
    // only thing the feedback-loop guard tests. Declared here rather than
    // beside the AddMemory call because the WRITE and the READ have to
    // agree exactly, and one of them silently drifting is unrecoverable:
    // gossip would re-seed from gossip without bound.
    inline constexpr const char* kOwnOutputTag = "ne_gossip";

    void Initialize();

    // Run ONE sweep against the game time the tick was scheduled for.
    // Step 1 of the gossip tick job.
    //
    // Carries no cadence of its own — the scheduler owns that, and the
    // owed-sweep backlog that used to live here now lives in the job
    // queue as stamped ticks. Returns false if the sweep could not run
    // (graph or memory system not ready), which is the scheduler's cue to
    // leave the boundary owed rather than consume it.
    //
    // `asOfGameDay` is a HORIZON as well as a timestamp: memories written
    // after it are not examined, so a tick that runs late still harvests
    // the world as it stood when it was due.
    bool RunSweep(const GossipThread::Token&, double asOfGameDay, const GossipDispatch::CancellationHandle& cancel);

    // Session totals, reset on OnSessionStart. Per-sweep numbers go to
    // the gossip trace as HARVEST lines — the two are kept apart on
    // purpose, since a cumulative count and a per-sweep count printed
    // side by side cannot be read as a rate.
    struct Stats
    {
        std::size_t sweeps = 0;
        std::size_t actorsExamined = 0;
        std::size_t memoriesExamined = 0;
        // Candidates placed in the evaluation pool, which is NOT the
        // number of rumors, the number of LLM calls, or even the number
        // of memories claimed. The sweep hands the pool over shuffled and
        // the walk stops at the first acceptance, so most of a pool is
        // typically never reached — and what it never reaches it never
        // claims. The per-candidate outcomes are the `content:` lines in
        // the trace.
        std::size_t sentForGeneration = 0;
        // Rows the query asked SkyrimNet to exclude and got back anyway.
        // 0 on a healthy v10+ install; anything else means the
        // server-side filter is not holding.
        std::size_t rejectedUnfiltered = 0;
        std::size_t rejectedClaimed = 0;
        std::size_t rejectedDiary = 0;
        std::size_t rejectedNoContent = 0;
        // Another witness's account of the same happening already seeded.
        std::size_t rejectedSameEvent = 0;
        // The origin's contacts are mostly unreachable, so it could not
        // have spread the rumor anywhere.
        std::size_t rejectedIsolated = 0;
    };
    Stats GetStats(const GossipState&);
} // namespace NarrativeEngine::GossipHarvest
