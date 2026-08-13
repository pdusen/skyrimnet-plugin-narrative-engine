#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <GossipDispatch.h>
#include <GossipState.h>
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
// Real AddMemory calls, composed by GossipContent from the rumor's
// generation-banded text plus relationship-aware framing. No LLM is
// involved anywhere in this module: the calls that judge the memory and
// produce the band text both happen at seed time, before the rumor is
// ever handed here.
//
// The two sides are written on different schedules. A listener catches a
// rumor once and is immune afterwards, so their memory goes out as the
// transmission happens. A carrier can pass the same rumor to several
// people in one tick, so the teller side accumulates and is written once
// per (rumor, carrier) at the end of the drain, naming everyone they told
// — "I told Onmund, Nirya and Tolfdir this: ..." rather than three
// near-identical rows.
//
// Every memory is typed KNOWLEDGE and tagged GossipHarvest::kOwnOutputTag,
// which is what keeps gossip's own output out of GossipHarvest's
// candidate set.
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

    // The live state instance.
    //
    // GossipClaims binds its ledger references into this, because claims
    // and rumors must be snapshotted — and therefore saved — from the
    // same instant. Exposed rather than duplicated so there is exactly
    // one object to copy when a tick publishes.
    //
    // Milestone 3 step 7 replaces this with a GossipThread::Token-gated
    // accessor; until the work has actually moved threads, the existing
    // mutex is still what guards it.
    // The live state.
    //
    // Gated on GossipThread::Token, which is the whole of Milestone 3's
    // safety argument: the gossip worker is the only thread that can
    // produce one, so it is the only thread that can reach this, so the
    // simulation needs no mutex. AsyncDispatch, the main thread and
    // SKSE's serialisation thread have no way to call it — a compile
    // error, not a rule.
    //
    // Everything they need instead: Snapshot() to read, PendingState() to
    // write.
    GossipState& MutableState(const GossipThread::Token&);

    // The last published image of the whole gossip world.
    //
    // Immutable and shared: every outside reader — the dashboard, the
    // co-save, the session-end census — loads this pointer and reads it
    // with no lock and no wait. Never null; before the first publish it
    // is an empty state, which reads correctly as "nothing has happened
    // yet".
    //
    // Published only at the end of a unit of gossip work, never during
    // one. A snapshot taken mid-drain would show a half-advanced
    // simulation: some carriers stepped to the new game day and some
    // not, transmission counts that do not match the carrier set they
    // came from. What a reader needs is a series of consistent states,
    // and a completed unit of work is exactly that.
    std::shared_ptr<const GossipState> Snapshot();

    // Copy the live state into a new published snapshot. Called at the
    // end of gossip work; step 5 of Milestone 3 narrows that to once per
    // scheduled tick.
    void PublishSnapshot();

    // kNewGame / kPostLoadGame. Refreshes the graph's relationship
    // layer and re-bases the game-time sample so a load does not look
    // like a colossal time jump.
    void OnSessionStart();

    // kPreLoadGame. Writes the live-rumor census into the gossip log
    // before GossipLog closes its file.
    void OnSessionEnd();

    // Stamp the simulation clock with the tick's horizon. Step 0 of the
    // tick job — before the harvest, because a rumor seeded during this
    // tick dates itself from this clock.
    void SetHorizon(const GossipThread::Token&, double asOfGameDay);

    // Advance the simulation to `asOfGameDay` and drain everything due by
    // then. Step 3 of the gossip tick job.
    //
    // The clock is SET rather than advanced by a sampled delta, which is
    // what lets a tick that ran late still be a correct tick: it
    // simulates up to the moment it was scheduled for and no further.
    //
    // Runs to completion — no time budget, because nothing else runs on
    // the gossip thread and there is nobody to yield to. `cancel` is
    // polled between events; a tick abandoned mid-drain stops writing
    // memories into a world that has been replaced under it.
    void Advance(const GossipThread::Token&, double asOfGameDay, const GossipDispatch::CancellationHandle& cancel);

    // Introduce a rumor originating with `originNpc`, sourced from
    // `sourceMemoryId`. Returns the rumor's id, or 0 if it could not be
    // seeded (graph not ready, origin not a participant, or the
    // live-rumor cap is full).
    // `bands` is the generation-banded text, produced at seed time by
    // GossipContent. Band selection at transmission time is by the
    // receiving carrier's generation.
    std::uint32_t SeedRumor(const GossipThread::Token&,
                            RE::FormID originNpc,
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
    // Projections take the image to read EXPLICITLY, so a call site
    // cannot be ambiguous about whether it is looking at the published
    // snapshot or at live state. The dashboard wants the snapshot — one
    // image per compose, so its numbers cannot disagree with each other.
    // GossipContent wants live state, because a rumor seeded moments ago
    // belongs in the list the next candidate is judged against.
    Stats GetStats(const GossipState&);

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
        // fGossipProvinceShare) that can reach any participant at all,
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
    float AvailableContactShare(const GossipThread::Token&, RE::FormID npc);

    // Why AvailableContactShare returned what it did, as one trace line:
    // the tier locations the graph gave `npc`, how much of their contact
    // weight is reachable, and the unreachable peers responsible, largest
    // share first, each named with the channel that put them there.
    //
    // Exists because the verdict is a weighted ratio over data assembled
    // from three different places -- the LCUN walk, the faction pairs and
    // the live availability read -- and knowing only the ratio does not
    // say which of them produced it. Ancano reads 1% while standing in a
    // College full of live mages, and the records alone cannot distinguish
    // "his settlement never resolved" from "his faction edges outweigh it"
    // from "his neighbours are not resolving as available". All three are
    // one line of numbers apart.
    //
    // Rebuilds the same walk rather than sharing it, so call it only on
    // the rejection path. Contacts are cached; the availability reads are
    // not, and there is one per peer.
    std::string DescribeContactAvailability(const GossipThread::Token&, RE::FormID npc);

    // Every rumor still in the map, newest first. The map holds exactly
    // the rumors that have not been reaped, which is what the dashboard
    // wants: a rumor is listed until its last carrier retires.
    std::vector<RumorView> GetRumorViews(const GossipState&);

    // Serialisation never touches live state and never waits on the
    // gossip worker.
    //
    // OnSave writes the image it is handed — the caller takes ONE
    // snapshot and passes it to both records, so claims and rumors are
    // always saved from the same instant. Split them and a reload can
    // restore a rumor whose source memory is no longer claimed, leaving
    // that memory free to seed a second rumor about a happening already
    // going round.
    void OnSave(SKSE::SerializationInterface* intfc, const GossipState& state);

    // OnLoad and OnRevert write into a PENDING state rather than the live
    // one, and the worker adopts it at the top of its next unit of work.
    // Neither blocks; the serialisation thread publishes inward exactly
    // as the worker publishes outward.
    //
    // Each writes only its own portion of the pending state, so the order
    // SKSE happens to dispatch the two records in cannot matter.
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();

    // The staging area OnLoad and OnRevert write into. Created empty on
    // first touch.
    GossipState& PendingState();

    // Move any staged state into the live one and publish it. Returns
    // true if there was something to adopt.
    //
    // Called at the top of every unit of gossip work AND at
    // OnSessionStart, which runs at kPostLoadGame — after the record
    // dispatch that filled the staging area, and before anything re-bases
    // the simulation clock against state that is about to be replaced.
    bool AdoptPendingState();
} // namespace NarrativeEngine::GossipSim
