#include <GossipSim.h>

#include <GossipDispatch.h>

#include <optional>

#include <GossipState.h>

#include <EventLogUtil.h>
#include <GossipClaims.h>
#include <GossipContent.h>
#include <GossipGraph.h>
#include <GossipHarvest.h>
#include <GossipLog.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NarrativeEngine::GossipSim
{
    namespace
    {
        // v6 adds the harvest bucket selection history.
        constexpr std::uint32_t kRecordVersion = 7;

        // Sentinel peer standing in for "somebody, anywhere in Skyrim".
        // Resolved to a random participant only if it is actually
        // selected — materialising ~850 province edges per carrier to
        // model a 0.0001-weight channel would be absurd.
        constexpr RE::FormID kProvincePeer = 0xFFFFFFFF;

        // Rumor, Carrier, QueueEntry and EventQueue now live in
        // GossipState.h, because the co-save and the dashboard both need
        // to read them from a published snapshot rather than from live
        // state. The `using` keeps every reference in this file spelled
        // the way it always was.
        using namespace GossipState_;

        // THE live instance. Everything below is a reference into it, so
        // that publishing a snapshot is one copy of one object rather
        // than a gather across six globals that could each be caught at a
        // different instant.
        //
        // A function-local static rather than a namespace-scope object,
        // because GossipClaims binds references into it during ITS static
        // initialisation and the order between two translation units is
        // unspecified. Construct-on-first-use removes the question
        // instead of leaving it to be reasoned about.
        GossipState& State()
        {
            static GossipState state;
            return state;
        }

        auto& g_rumors = State().rumors;
        auto& g_queue = State().queue;
        auto& g_nextRumorId = State().nextRumorId;
        auto& g_counters = State().counters;

        auto& g_lastGameDaySample = State().lastGameDaySample;
        auto& g_simGameDay = State().simGameDay;

        // The published image. Swapped wholesale; readers hold their copy
        // of the shared_ptr for as long as they need it, so a publish can
        // never pull the ground out from under a reader mid-read.
        //
        // Guarded by its own mutex rather than std::atomic<shared_ptr>
        // purely for MSVC-version portability; the critical section is a
        // pointer assignment.
        std::mutex g_snapshotMutex;
        std::shared_ptr<const GossipState> g_snapshot = std::make_shared<const GossipState>();

        // The inward channel. SKSE's serialisation thread writes here;
        // the worker adopts it at the top of its next unit of work. The
        // two never wait on each other — states go in, snapshots come
        // out.
        std::mutex g_pendingMutex;
        std::optional<GossipState> g_pending;

        std::mt19937 g_rng{1337};

        // Per-carrier contact set, memoised for the session. The graph
        // is immutable between session starts, so a carrier's weighted
        // contact list never changes and recomputing it per event would
        // be pure waste.
        // The contact ladder, seven rungs. Rungs 0/2/4/6 (household,
        // settlement, hold, province in the doc's 1-based numbering) are the
        // geographic tiers; rungs 1/3/5 have no natural membership and hold
        // only peers a faction or relationship has moved.
        enum Rung : int
        {
            kHousehold = 0,
            kSettlement = 2,
            kHold = 4,
            kProvince = 6,
            kRungCount = 7,
        };

        const char* RungName(int rung)
        {
            switch (rung) {
            case 0:
                return "household";
            case 1:
                return "t2";
            case 2:
                return "settlement";
            case 3:
                return "t4";
            case 4:
                return "hold";
            case 5:
                return "t6";
            default:
                return "province";
            }
        }

        // A carrier's rungs, materialised. The province rung is deliberately
        // absent: it stands for every participant in Skyrim and is resolved
        // by drawing one at random rather than by storing 800-odd ids per
        // carrier.
        //
        // Cached because the graph is immutable between session starts. This
        // replaces the old flat weighted contact list, and is far smaller:
        // each peer appears once, with no per-pair rate to store.
        using TierPools = std::array<std::vector<RE::FormID>, kRungCount - 1>;
        std::unordered_map<RE::FormID, TierPools> g_poolCache;

        double NowGameDay()
        {
            return EventLogUtil::NowGameTimeSeconds() / 86400.0;
        }

        float UniformFloat(float lo, float hi)
        {
            std::uniform_real_distribution<float> d(lo, hi);
            return d(g_rng);
        }

        // Build the weighted contact list for one carrier and divide the
        // daily conversation budget among it.
        //
        // The division is the whole point: weights are RATIOS. A
        // 90-resident city and a two-person farmstead both yield roughly
        // fGossipConversationsPerDay contacts per day; the city dweller
        // simply spreads theirs more thinly.
        const TierPools& PoolsFor(RE::FormID npc)
        {
            if (const auto it = g_poolCache.find(npc); it != g_poolCache.end()) {
                return it->second;
            }

            TierPools pools{};
            const auto* self = GossipGraph::Find(npc);
            if (!self) {
                return g_poolCache.emplace(npc, std::move(pools)).first->second;
            }

            // Natural rung is the CLOSEST tier a peer qualifies for, so the
            // rungs stay disjoint and a housemate is not also drawn as a
            // settlement neighbour. Closest-first insertion does that without
            // a second pass.
            std::unordered_map<RE::FormID, int> natural;
            const auto claim = [&](RE::FormID peer, int rung) {
                if (peer != npc) {
                    natural.emplace(peer, rung);
                }
            };
            if (self->household) {
                for (const auto peer : GossipGraph::HouseholdMembers(self->household)) {
                    claim(peer, kHousehold);
                }
            }
            if (self->settlement) {
                for (const auto peer : GossipGraph::SettlementMembers(self->settlement)) {
                    claim(peer, kSettlement);
                }
            }
            if (self->hold) {
                for (const auto peer : GossipGraph::HoldMembers(self->hold)) {
                    claim(peer, kHold);
                }
            }

            const auto place = [&](RE::FormID peer, int rung) {
                // The province rung is virtual, so anyone who lands on it is
                // simply not stored: they remain reachable through the
                // province lottery like any other stranger.
                if (rung >= 0 && rung < kProvince) {
                    pools[static_cast<std::size_t>(rung)].push_back(peer);
                }
            };

            for (const auto& edge : GossipGraph::PersonalEdges(npc)) {
                if (const auto it = natural.find(edge.other); it != natural.end()) {
                    it->second = std::clamp(it->second + edge.tierDelta, 0, static_cast<int>(kProvince));
                } else if (edge.tierDelta < 0) {
                    // No shared geography, so their natural rung is province;
                    // moved one closer they land on rung 6 -- in a pool of
                    // their own rather than mixed into the carrier's hold,
                    // which is the entire reason the odd rungs exist. A
                    // measured 12% of rumors cross a hold boundary through
                    // this path; under the old flat model it was 1%.
                    natural.emplace(edge.other, kProvince - 1);
                }
            }
            for (const auto& [peer, rung] : natural) {
                place(peer, rung);
            }

            return g_poolCache.emplace(npc, std::move(pools)).first->second;
        }

        // A prospective participant must still resolve to a live base
        // form. Checked here rather than at graph-build time so a form
        // removed by a load-order change drops out without a rebuild.
        //
        // Whether an actor can hold a conversation right now, and whether
        // that is a permanent state.
        //
        // Only kAlive counts as available: someone bleeding out, unconscious
        // or restrained cannot gossip either. But "cannot gossip right now"
        // and "out of the epidemic" are different outcomes, and conflating
        // them is a real bug — retiring a carrier under SIR makes them
        // PERMANENTLY immune, so an NPC knocked out for a day would be
        // silently removed from the outbreak for good instead of resuming.
        //
        // This is possible off the main thread because unique NPCs' Actor
        // objects are persistent and always resident; only their 3D unloads.
        // BGSLocation stores them as UniqueNPCData { Actor* actor; ... }
        // while storing ordinary persistent refs as UnloadedRefData
        // { FormID refID; FormID parentSpaceID; CellKey cellKey; } — the
        // engine would not hold a raw pointer to something destroyed on cell
        // unload. That is why GetDead works as a condition on them anywhere.
        //
        // LookupByID takes the engine's own read-write lock and
        // GetLifeState() is an inline field read, so neither needs the main
        // thread.
        enum class Availability : std::uint8_t
        {
            Available,      // kAlive
            TemporarilyOut, // down, restrained, disabled — will be back
            Gone,           // dead, or the form no longer resolves
        };

        Availability ActorAvailability(RE::FormID npc)
        {
            const auto* p = GossipGraph::Find(npc);
            if (!p || p->actorRef == 0) {
                return Availability::Gone;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(p->actorRef);
            if (!actor) {
                return Availability::Gone;
            }
            // AsActorState() rather than the inherited GetLifeState(): the
            // ActorState base sits at a different offset on AE (0xC0) than
            // SE (0xB8), and AsActorState does that relocation. Calling the
            // inherited method directly reads the wrong bytes on one runtime.
            const auto* state = actor->AsActorState();
            if (!state) {
                return Availability::Gone;
            }
            switch (state->GetLifeState()) {
            case RE::ACTOR_LIFE_STATE::kAlive:
                // Disabled is transient: a quest can re-enable them.
                return actor->IsDisabled() ? Availability::TemporarilyOut : Availability::Available;
            case RE::ACTOR_LIFE_STATE::kDead:
                return Availability::Gone;
            default:
                // kDying resolves itself — transient now, kDead next step.
                return Availability::TemporarilyOut;
            }
        }

        void ScheduleLocked(std::uint32_t rumorId, RE::FormID carrier, double dueGameDay)
        {
            g_queue.push({dueGameDay, rumorId, carrier});
        }

        // Teller-side memories, accumulated across the tick and written
        // once per (rumor, carrier) when the drain ends.
        //
        // A listener catches a given rumor exactly once — the carrier map
        // makes them immune afterwards — so the listener side is naturally
        // one memory per person and is written immediately. The TELLER side
        // is not: one carrier can pass the same rumor to several people in
        // a tick, which used to produce that many near-identical "I told X
        // this: ..." rows. Collapsing them into "I told X, Y and Z this:"
        // reads better and writes fewer rows into the store the harvester
        // has to read back through.
        //
        // Keyed on (rumor, teller). An ordered map rather than a hash so the
        // pair key needs no hash specialisation; it holds at most one entry
        // per carrier that spoke this tick.
        struct PendingTell
        {
            std::vector<RE::FormID> listeners; // base forms, in telling order
            std::size_t band = 0;
            // The TELLER's own location at their first telling of the tick.
            // A per-listener location cannot be used once there are several
            // listeners, and where the teller stood is the more sensible
            // answer for a memory about telling.
            RE::FormID location = 0;
        };
        std::map<std::pair<std::uint32_t, RE::FormID>, PendingTell> g_pendingTells;

        // The memories a transmission writes. No LLM here — this is a
        // string build from band text the seed-time call already produced,
        // plus relationship-aware framing.
        //
        // The Milestone 1 [NE-GOSSIP-STUB ...] prefix and the "stub" tag are
        // gone. What marks these as ours is GossipHarvest::kOwnOutputTag,
        // which the harvester tests and nothing else writes — SkyrimNet's
        // own tagger uses a plain "gossip" tag for memories that are merely
        // ABOUT gossiping, so sharing that name made the guard discard real
        // material. Type stays KNOWLEDGE.
        //
        // `teller` and `listener` arrive as TESNPC BASE forms, because that
        // is what the carrier map and the whole graph are keyed on. Every
        // id handed to SkyrimNet — the memory owner and the related-actor
        // array alike — must be the PLACED REFERENCE instead. These are
        // different FormIDs for the same person, and writing a memory
        // against the base form addresses nobody.
        void RecordTransmission(const Rumor& rumor,
                                RE::FormID teller,
                                RE::FormID listener,
                                std::uint32_t generation,
                                RE::FormID listenerLocation,
                                RE::FormID tellerLocation)
        {
            if (rumor.bands.empty()) {
                return;
            }
            const auto tellerRef = GossipGraph::ActorRefFor(teller);
            const auto listenerRef = GossipGraph::ActorRefFor(listener);
            if (tellerRef == 0 || listenerRef == 0) {
                // A participant the LCUN walk gave no reference for. The
                // transmission still happened in the model — it is only the
                // memory write that cannot be addressed — so this counts as
                // a write failure rather than unwinding the simulation.
                ++g_counters.memoryWriteFailures;
                if (Settings::Get().debugMode) {
                    logger::debug("GossipSim: no placed reference for {} -> {}; memory pair skipped",
                                  tellerRef == 0 ? GossipGraph::NpcName(teller) : GossipGraph::NpcName(listener),
                                  rumor.id);
                }
                return;
            }

            const auto band = std::min(GossipContent::BandForGeneration(generation), rumor.bands.size() - 1);
            const auto tags = std::format(R"(["{}"])", GossipHarvest::kOwnOutputTag);

            // The listener's memory goes out now: they hear this rumor once
            // and never again, so there is nothing to accumulate.
            const int written =
                SkyrimNetAPI::AddMemory(listenerRef,
                                        GossipContent::ComposeHeard(rumor.bands[band], teller, listener),
                                        rumor.notability,
                                        "KNOWLEDGE",
                                        "",
                                        GossipGraph::LocationName(listenerLocation),
                                        tags,
                                        std::format("[{}]", tellerRef));
            if (written > 0) {
                ++g_counters.memoriesWritten;
            } else {
                ++g_counters.memoryWriteFailures;
            }

            // The teller's waits for the end of the tick, by which point
            // everyone they told is known.
            auto& pending = g_pendingTells[{rumor.id, teller}];
            if (pending.listeners.empty()) {
                pending.band = band;
                pending.location = tellerLocation;
            }
            pending.listeners.push_back(listener);
        }

        // One teller-side memory per (rumor, carrier) that spoke, then empty
        // the accumulator.
        //
        // Must run on EVERY path out of the drain, cancellation included.
        // The listener halves are already written, so skipping this would
        // leave them unanswered — and worse, would carry the accumulator
        // into the next tick and credit this tick's listeners to that one.
        void FlushPendingTells()
        {
            for (const auto& [key, pending] : g_pendingTells) {
                const auto rumorId = key.first;
                const auto teller = key.second;
                const auto it = g_rumors.find(rumorId);
                if (it == g_rumors.end() || pending.listeners.empty()) {
                    continue;
                }
                const auto& rumor = it->second;
                if (pending.band >= rumor.bands.size()) {
                    continue;
                }
                const auto tellerRef = GossipGraph::ActorRefFor(teller);
                if (tellerRef == 0) {
                    ++g_counters.memoryWriteFailures;
                    continue;
                }

                std::string related = "[";
                for (const auto listener : pending.listeners) {
                    const auto ref = GossipGraph::ActorRefFor(listener);
                    if (ref == 0) {
                        continue;
                    }
                    if (related.size() > 1) {
                        related += ",";
                    }
                    related += std::format("{}", ref);
                }
                related += "]";

                const int written =
                    SkyrimNetAPI::AddMemory(tellerRef,
                                            GossipContent::ComposeTold(rumor.bands[pending.band], pending.listeners),
                                            rumor.notability,
                                            "KNOWLEDGE",
                                            "",
                                            GossipGraph::LocationName(pending.location),
                                            std::format(R"(["{}"])", GossipHarvest::kOwnOutputTag),
                                            related);
                if (written > 0) {
                    ++g_counters.memoriesWritten;
                } else {
                    ++g_counters.memoryWriteFailures;
                }
            }
            g_pendingTells.clear();
        }

        void FinishRumorLocked(Rumor& rumor)
        {
            rumor.live = false;

            std::unordered_set<RE::FormID> holds;
            std::unordered_set<RE::FormID> settlements;
            for (const auto& [npc, _] : rumor.carriers) {
                if (const auto* p = GossipGraph::Find(npc)) {
                    if (p->hold) {
                        holds.insert(p->hold);
                    }
                    if (p->settlement) {
                        settlements.insert(p->settlement);
                    }
                }
            }

            GossipLog::BurnoutStats stats;
            stats.reach = rumor.carriers.size();
            stats.depth = rumor.maxDepth;
            stats.holds = holds.size();
            stats.settlements = settlements.size();
            stats.days = std::max(0.0, rumor.lastActivityGameDay - rumor.seedGameDay);
            stats.transmissions = rumor.transmissions;
            stats.wasted = rumor.wasted;
            stats.conversations = rumor.conversations;
            stats.notCaught = rumor.notCaught;
            stats.unavailable = rumor.unavailable;
            stats.capped = rumor.capped;
            stats.silent = rumor.silent;
            GossipLog::Burnout(rumor.id, stats);
        }

        // One simulation step for one infectious carrier.
        //
        // The carrier holds Poisson(conversationsPerDay * stepDays)
        // conversations. Each partner is drawn in proportion to that pair's
        // contact weight, so a housemate comes up far more often than any one
        // settlement neighbour does.
        //
        // Every conversation is simulated, including the ones that land on
        // somebody who already knows. Those wasted conversations ARE the
        // termination mechanism — the outbreak ends because susceptible
        // contacts run out, not because the rumor wore out. This is why the
        // scheduling cannot be thinned by the transmission probability the way
        // an earlier revision did: thinning hides exactly the events that stop
        // the epidemic.
        void ProcessEventLocked(std::uint32_t rumorId, RE::FormID carrierId, double nowGameDay)
        {
            const auto rit = g_rumors.find(rumorId);
            if (rit == g_rumors.end() || !rit->second.live) {
                return;
            }
            auto& rumor = rit->second;

            const auto cit = rumor.carriers.find(carrierId);
            if (cit == rumor.carriers.end() || cit->second.recovered) {
                return;
            }
            auto& carrier = cit->second;

            const auto& cfg = Settings::Get();

            const auto recover = [&](const char* reason) {
                carrier.recovered = true;
                GossipLog::Retire(rumorId, carrierId, reason);
                const bool anyLive = std::any_of(
                    rumor.carriers.begin(), rumor.carriers.end(), [](const auto& kv) { return !kv.second.recovered; });
                if (!anyLive) {
                    FinishRumorLocked(rumor);
                }
            };

            if (nowGameDay >= carrier.infectiousUntilGameDay) {
                recover("recovered");
                return;
            }
            if (nowGameDay - carrier.heardOnGameDay > cfg.gossipCarrierMaxAgeDays) {
                recover("age");
                return;
            }
            switch (ActorAvailability(carrierId)) {
            case Availability::Gone:
                recover("dead");
                return;
            case Availability::TemporarilyOut:
                // Down but not out. Skip this step and try again next one;
                // do NOT recover, which would make them permanently immune.
                // Their infectious clock keeps running — time spent
                // unconscious is time not spent talking, which is correct
                // and needs no clock-pausing machinery.
                carrier.nextStepGameDay = nowGameDay + std::max(0.01f, cfg.gossipStepDays);
                ScheduleLocked(rumorId, carrierId, carrier.nextStepGameDay);
                return;
            case Availability::Available:
                break;
            }

            const auto& pools = PoolsFor(carrierId);
            const auto& weights = cfg.gossipTierWeights;
            float totalWeight = 0.0f;
            for (const auto w : weights) {
                totalWeight += std::max(0.0f, w);
            }
            // Every rung zeroed is a configuration with no contact model at
            // all; treat it as having nobody rather than dividing by zero.
            if (totalWeight <= 0.0f) {
                recover("no-contacts");
                return;
            }
            // A carrier with nothing on any materialised rung can still reach
            // strangers through the province lottery, so "no contacts" now
            // means only that the province rung is also switched off.
            const bool anyLocal = std::any_of(pools.begin(), pools.end(), [](const auto& p) { return !p.empty(); });
            if (!anyLocal && weights[kProvince] <= 0.0f) {
                recover("no-contacts");
                return;
            }

            const double step = std::max(0.01f, cfg.gossipStepDays);
            const double beta =
                std::clamp(static_cast<double>(rumor.notability * cfg.gossipTransmissionScale), 0.0, 1.0);
            // Deterministic per step, not Poisson. The variance that matters
            // now comes from which rung is drawn and who is on it; layering a
            // second source on the count only blurred the tier mix.
            const int conversations = std::max(0, static_cast<int>(std::lround(cfg.gossipConversationsPerStep)));
            rumor.conversations += static_cast<std::size_t>(conversations);

            std::uniform_real_distribution<float> pickRung(0.0f, totalWeight);
            std::uniform_real_distribution<double> roll01(0.0, 1.0);
            const auto* fromP = GossipGraph::Find(carrierId);

            for (int i = 0; i < conversations; ++i) {
                // Rung first, peer second. This is the whole point of the
                // model: the channel mix is chosen rather than emerging from
                // how many people happen to live in each tier.
                float roll = pickRung(g_rng);
                int rung = kRungCount - 1;
                for (int r = 0; r < kRungCount; ++r) {
                    roll -= std::max(0.0f, weights[static_cast<std::size_t>(r)]);
                    if (roll <= 0.0f) {
                        rung = r;
                        break;
                    }
                }

                RE::FormID listener = 0;
                if (rung == kProvince) {
                    const auto& all = GossipGraph::Participants();
                    if (all.empty()) {
                        ++rumor.silent;
                        ++g_counters.silent;
                        continue;
                    }
                    std::uniform_int_distribution<std::size_t> d(0, all.size() - 1);
                    listener = all[d(g_rng)];
                    if (listener == carrierId) {
                        ++rumor.silent;
                        ++g_counters.silent;
                        continue;
                    }
                } else {
                    const auto& pool = pools[static_cast<std::size_t>(rung)];
                    if (pool.empty()) {
                        // Nobody on this rung. The draw is spent and nothing
                        // is said; the remaining rungs are NOT renormalised
                        // to compensate, so a carrier with no household
                        // simply talks less than one who has a family.
                        ++rumor.silent;
                        ++g_counters.silent;
                        continue;
                    }
                    std::uniform_int_distribution<std::size_t> d(0, pool.size() - 1);
                    listener = pool[d(g_rng)];
                }

                if (listener == carrierId || rumor.carriers.contains(listener)) {
                    // Already infectious, or recovered and immune. A wasted
                    // opportunity — the brake.
                    ++rumor.wasted;
                    ++g_counters.wasted;
                    GossipLog::Wasted(rumorId, carrierId, listener, conversations - i - 1);
                    continue;
                }

                if (roll01(g_rng) >= beta) {
                    // They spoke and it did not take. Counted rather than
                    // dropped: this is the single largest silent outcome,
                    // and its absence made "reached nobody" unattributable.
                    ++rumor.notCaught;
                    ++g_counters.notCaught;
                    continue;
                }

                const auto* toP = GossipGraph::Find(listener);
                if (!toP || ActorAvailability(listener) != Availability::Available) {
                    ++rumor.unavailable;
                    ++g_counters.unavailable;
                    // No conversation happened. Deliberately NOT counted as a
                    // wasted telling: wasted tellings are the saturation brake
                    // and mean "they already knew", which is a different
                    // thing. Miscounting here would make the outbreak look
                    // more saturated than it is and terminate early. The
                    // listener stays susceptible and can catch it later.
                    continue;
                }
                if (static_cast<int>(rumor.carriers.size()) >= cfg.gossipMaxCarriersPerRumor) {
                    ++rumor.capped;
                    ++g_counters.capped;
                    continue;
                }

                Carrier fresh;
                fresh.generation = carrier.generation + 1;
                fresh.toldBy = carrierId;
                fresh.heardOnGameDay = nowGameDay;
                fresh.infectiousUntilGameDay = nowGameDay + std::max(0.1f, cfg.gossipInfectiousDays);
                fresh.nextStepGameDay = nowGameDay + step;

                rumor.carriers.emplace(listener, fresh);
                rumor.maxDepth = std::max(rumor.maxDepth, fresh.generation);
                ++rumor.transmissions;
                ++g_counters.transmissions;
                rumor.lastActivityGameDay = nowGameDay;

                const RE::FormID location = toP->settlement ? toP->settlement : toP->hold;
                GossipLog::Tell(rumorId,
                                fresh.generation,
                                rumor.notability,
                                carrierId,
                                listener,
                                RungName(rung),
                                location,
                                fromP ? fromP->hold : 0,
                                toP->hold);

                RecordTransmission(rumor,
                                   carrierId,
                                   listener,
                                   fresh.generation,
                                   location,
                                   fromP ? (fromP->settlement ? fromP->settlement : fromP->hold) : 0);
                ScheduleLocked(rumorId, listener, fresh.nextStepGameDay);
            }

            carrier.nextStepGameDay = nowGameDay + step;
            ScheduleLocked(rumorId, carrierId, carrier.nextStepGameDay);
        }

        // Claim expiry and burned-out-rumor reaping, run at the end of every
        // poll rather than once the map grows past some threshold.
        //
        // Erasing here rather than in FinishRumorLocked is deliberate:
        // ProcessEventLocked holds references into the map while it runs, and
        // erasing underneath it would invalidate them. Stale queue entries
        // pointing at a reaped rumor are harmless — ProcessEventLocked looks
        // the id up and returns when it is gone.
        //
        // A burned-out rumor is pure dead weight: every participant already
        // has their memories in SkyrimNet's database, no carrier is scheduled,
        // and nothing anywhere reads a dead rumor's data. Its carrier map —
        // up to gossipMaxCarriersPerRumor entries at ~37 bytes each — would
        // otherwise ride along in the co-save.
        void SweepAndReap(const GossipThread::Token& gt)
        {
            // Claim expiry rides the same sampled game time as the rumor reap.
            // Deliberately not gated on there being live rumors: a quiet
            // stretch still has to let claims age out, or a lull would freeze
            // the ledger.
            if (const auto expired = GossipClaims::Sweep(gt, g_simGameDay); expired > 0) {
                logger::debug("GossipClaims: expired {} claim(s); {} memory + {} event claim(s) remain",
                              expired,
                              GossipClaims::Count(State()),
                              GossipClaims::EventCount(State()));
            }

            std::size_t reaped = 0;
            for (auto it = g_rumors.begin(); it != g_rumors.end();) {
                if (it->second.live) {
                    ++it;
                } else {
                    it = g_rumors.erase(it);
                    ++reaped;
                }
            }
            if (reaped > 0 && Settings::Get().debugMode) {
                logger::debug("GossipSim: reaped {} burned-out rumor(s); {} remain", reaped, g_rumors.size());
            }
        }

        // Has this rumor run out of people to tell?
        //
        // True when every still-infectious carrier finds everyone on their
        // materialised rungs already carrying it. The rumor is alive — carriers are
        // still scheduled, still burning down their infectious window — but
        // there is nobody left for them to reach.
        //
        // The province rung is skipped on purpose. It stands for "somebody,
        // anywhere in Skyrim", is resolved to a random participant at
        // transmission time, and every carrier has it. Counting it as a
        // vector would make this predicate answer "not stalled" for every
        // rumor until literally every participant in the province carried it,
        // which is never. What the reader wants to know is whether the
        // rumor's local social neighbourhood is saturated, and that is what
        // this measures. The province lottery can still fire and un-stall a
        // rumor; that is the model working, not the readout lying.
        //
        // Cost is bounded and early-exits on the first susceptible contact,
        // which is the overwhelmingly common case for a spreading rumor. Only
        // a genuinely stalled rumor pays the full scan.
        bool IsStalled(const Rumor& rumor)
        {
            for (const auto& [npc, carrier] : rumor.carriers) {
                if (carrier.recovered) {
                    continue;
                }
                for (const auto& pool : PoolsFor(npc)) {
                    for (const auto peer : pool) {
                        if (!rumor.carriers.contains(peer)) {
                            return false; // somebody left to tell
                        }
                    }
                }
            }
            // No infectious carriers at all means it is not going anywhere
            // either — though the reap normally removes such a rumor in the
            // same poll that produced it.
            return true;
        }
    } // namespace

    GossipState& MutableState(const GossipThread::Token&)
    {
        return State();
    }

    std::shared_ptr<const GossipState> Snapshot()
    {
        std::scoped_lock lock(g_snapshotMutex);
        return g_snapshot;
    }

    GossipState& PendingState()
    {
        std::scoped_lock lock(g_pendingMutex);
        if (!g_pending) {
            g_pending.emplace();
        }
        return *g_pending;
    }

    bool AdoptPendingState()
    {
        std::optional<GossipState> taken;
        {
            std::scoped_lock lock(g_pendingMutex);
            taken.swap(g_pending);
        }
        if (!taken) {
            return false;
        }
        {
            State() = std::move(*taken);
            // Derived, not owned, and describing a world that may have
            // just been replaced.
            g_poolCache.clear();
        }
        PublishSnapshot();
        logger::debug(
            "GossipSim: adopted pending state ({} rumors, {} claims)", State().rumors.size(), State().claims.size());
        return true;
    }

    void PublishSnapshot()
    {
        auto fresh = std::make_shared<const GossipState>(State());
        std::scoped_lock lock(g_snapshotMutex);
        g_snapshot = std::move(fresh);
    }

    void Initialize()
    {
        const auto& cfg = Settings::Get();
        if (cfg.gossipRandomSeed != 0) {
            g_rng.seed(static_cast<std::uint32_t>(cfg.gossipRandomSeed));
        } else {
            std::random_device rd;
            g_rng.seed(rd());
        }
        logger::info("GossipSim: initialized (enabled={}, rngSeed={})", cfg.gossipEnabled, cfg.gossipRandomSeed);
    }

    void OnSessionStart()
    {
        if (!Settings::Get().gossipEnabled) {
            return;
        }
        GossipGraph::RefreshRelationships();

        // Runs on the MAIN thread, so it must not touch live state. A tick
        // cancelled a moment ago at kPreLoadGame is not necessarily a tick
        // that has finished — it stops at its next checkpoint, which may
        // be a blocking LLM call away. Writing live here would race it.
        //
        // Everything inbound goes through the staging area instead, and
        // the gossip thread adopts it at the top of its next job. Note
        // this MODIFIES the pending state rather than replacing it: the
        // record dispatch has already filled it by now.
        //
        // The clock re-base that used to live here is gone. Nothing needs
        // it: SetHorizon stamps the simulation clock from the tick's own
        // schedule, and GossipTick re-bases its schedule separately.
        auto& pending = PendingState();
        pending.counters = {};
        pending.harvest = {};
    }

    void OnSessionEnd()
    {
        // Main thread, at kPreLoadGame. Reads the published snapshot
        // rather than live state for the same reason OnSessionStart
        // stages instead of writing: a cancelled tick may still be
        // unwinding. The snapshot is immutable and cannot be pulled out
        // from under this.
        const auto snap = Snapshot();
        const auto& g_rumors = snap->rumors;
        const auto& g_counters = snap->counters;
        if (g_rumors.empty()) {
            return;
        }
        // The session-level counterpart of the per-rumor BURNOUT line: the
        // five outcomes sum to every conversation held this session, so a
        // quiet session says whether nobody spoke or nothing landed.
        const auto conversations = g_counters.transmissions + g_counters.wasted + g_counters.notCaught
                                   + g_counters.unavailable + g_counters.capped + g_counters.silent;
        GossipLog::Note(std::format("CENSUS  live rumors={}  conversations={} ({} told, {} knew, {} missed, "
                                    "{} away, {} capped, {} silent)  memories={} (failed {})",
                                    g_rumors.size(),
                                    conversations,
                                    g_counters.transmissions,
                                    g_counters.wasted,
                                    g_counters.notCaught,
                                    g_counters.unavailable,
                                    g_counters.capped,
                                    g_counters.silent,
                                    g_counters.memoriesWritten,
                                    g_counters.memoryWriteFailures));
        for (const auto& [id, rumor] : g_rumors) {
            const auto liveCarriers = std::count_if(
                rumor.carriers.begin(), rumor.carriers.end(), [](const auto& kv) { return !kv.second.recovered; });
            GossipLog::Note(std::format("CENSUS  r{:02} {}  carriers={} (live {})  depth={}  transmissions={}",
                                        id,
                                        rumor.live ? "LIVE" : "done",
                                        rumor.carriers.size(),
                                        liveCarriers,
                                        rumor.maxDepth,
                                        rumor.transmissions));
        }
    }

    void SetHorizon(const GossipThread::Token&, double asOfGameDay)
    {
        // The simulation clock is SET, not advanced by a sampled delta.
        //
        // This is what makes a late tick still a correct tick: the job was
        // scheduled for `asOfGameDay` and treats the world as standing at
        // exactly that moment, whether it ran on time or forty seconds
        // late behind two LLM calls.
        //
        // Set BEFORE the harvest rather than inside the drain, because a
        // rumor seeded during this tick stamps itself with the clock. Set
        // it afterwards and every rumor would be dated one whole interval
        // in the past.
        //
        // It also retires kMaxGameDayDeltaPerPoll. That clamp existed to
        // stop a console time jump crediting an unbounded burst in one
        // poll; the scheduler now caps how many ticks may be outstanding,
        // which bounds the same thing in ticks rather than days and does
        // it before the work is ever queued.
        g_simGameDay = asOfGameDay;
        g_lastGameDaySample = asOfGameDay;
    }

    void Advance(const GossipThread::Token& gt, double asOfGameDay, const GossipDispatch::CancellationHandle& cancel)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !GossipGraph::IsReady()) {
            return;
        }
        if (asOfGameDay < g_simGameDay) {
            return;
        }

        // Drain the due queue to completion. No wall-clock budget: nothing
        // else runs on this thread, so there is nobody to yield to. The
        // count cap survives purely as a runaway backstop — it costs
        // nothing and is the difference between a bug being slow and a bug
        // being a hang.
        //
        // Note an "event" is a carrier-STEP, not a conversation: each one
        // runs Poisson(conversationsPerDay * stepDays) conversations.
        const int cap = std::max(1, cfg.gossipMaxEventsPerTick);
        int processed = 0;
        while (!g_queue.empty() && g_queue.top().dueGameDay <= g_simGameDay) {
            if (processed >= cap) {
                GossipLog::Note(std::format("drain: stopped at the {}-event backstop with {} still due; "
                                            "iGossipMaxEventsPerTick is too low or something is looping",
                                            cap,
                                            g_queue.size()));
                break;
            }
            // THE cancellation checkpoint that matters. Every step past
            // this point can make two AddMemory calls into SkyrimNet's
            // database, which lives outside our co-save and is not rolled
            // back by loading an earlier game. A tick that keeps draining
            // after a load keeps writing memories into a world with no
            // record of the rumor that produced them.
            if (cancel && cancel->IsCancelled()) {
                GossipLog::Note(std::format("drain: abandoned after {} event(s) — the world it was "
                                            "simulating has been replaced",
                                            processed));
                FlushPendingTells();
                return;
            }
            const auto entry = g_queue.top();
            g_queue.pop();
            // Process AT the event's scheduled time, not at the current
            // simulated time. A carrier due on day 5.2 when the tick's
            // horizon is day 6.0 reschedules from 5.2, is immediately due
            // again, and works through its backlog inside this same loop.
            //
            // Using g_simGameDay instead would collapse each carrier's
            // backlog to a single event per tick, so a 24-hour wait would
            // advance every rumor by exactly one telling regardless of how
            // much time passed.
            //
            // The loop cannot spin: a carrier reschedules a full
            // gossipStepDays ahead every time, so it can only fire
            // 1/stepDays times per simulated day.
            ProcessEventLocked(entry.rumorId, entry.carrier, entry.dueGameDay);
            ++processed;
        }

        // Before the reap, which erases burned-out rumors: a carrier can
        // speak and its rumor burn out inside the same drain, and the flush
        // reads the rumor's band text.
        FlushPendingTells();
        SweepAndReap(gt);
    }

    std::uint32_t SeedRumor(const GossipThread::Token&,
                            RE::FormID originNpc,
                            float notability,
                            std::int64_t sourceMemoryId,
                            std::vector<std::string> bands)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !GossipGraph::IsReady()) {
            return 0;
        }
        const auto* p = GossipGraph::Find(originNpc);
        if (!p) {
            return 0;
        }

        const auto liveCount =
            std::count_if(g_rumors.begin(), g_rumors.end(), [](const auto& kv) { return kv.second.live; });
        if (liveCount >= cfg.gossipMaxLiveRumors) {
            // Not silent: this refusal throttled the first SIR validation run
            // to a third of its configured seeding rate, and took log analysis
            // rather than a log line to find.
            GossipLog::Note(std::format("seed refused - {} live rumors at the iGossipMaxLiveRumors cap", liveCount));
            return 0;
        }

        const double now = NowGameDay();
        if (g_lastGameDaySample < 0.0) {
            g_lastGameDaySample = now;
            g_simGameDay = now;
        }

        Rumor rumor;
        rumor.id = g_nextRumorId++;
        rumor.originNpc = originNpc;
        rumor.originSettlement = p->settlement ? p->settlement : p->hold;
        rumor.seedGameDay = g_simGameDay;
        rumor.lastActivityGameDay = g_simGameDay;
        rumor.notability = std::clamp(notability, 0.0f, 1.0f);
        rumor.sourceMemoryId = sourceMemoryId;
        rumor.sourceActor = originNpc;
        rumor.bands = std::move(bands);

        Carrier origin;
        origin.generation = 0;
        origin.heardOnGameDay = g_simGameDay;
        origin.infectiousUntilGameDay = g_simGameDay + std::max(0.1f, cfg.gossipInfectiousDays);
        origin.nextStepGameDay = g_simGameDay + std::max(0.01f, cfg.gossipStepDays);
        rumor.carriers.emplace(originNpc, origin);

        const auto id = rumor.id;
        ScheduleLocked(id, originNpc, origin.nextStepGameDay);
        g_rumors.emplace(id, std::move(rumor));

        GossipLog::Seed(id, notability, originNpc, p->settlement ? p->settlement : p->hold, sourceMemoryId);
        return id;
    }

    float AvailableContactShare(const GossipThread::Token&, RE::FormID npc)
    {
        const auto& weights = Settings::Get().gossipTierWeights;
        const auto& pools = PoolsFor(npc);

        float total = 0.0f;
        float reachable = 0.0f;
        for (std::size_t rung = 0; rung < pools.size(); ++rung) {
            const float w = std::max(0.0f, weights[rung]);
            if (w <= 0.0f) {
                continue;
            }
            total += w;
            const auto& pool = pools[rung];
            if (std::any_of(pool.begin(), pool.end(), [](RE::FormID peer) {
                    return ActorAvailability(peer) == Availability::Available;
                })) {
                reachable += w;
            }
        }
        return total > 0.0f ? reachable / total : 0.0f;
    }

    std::string DescribeContactAvailability(const GossipThread::Token&, RE::FormID npc)
    {
        const auto* self = GossipGraph::Find(npc);
        const auto& weights = Settings::Get().gossipTierWeights;
        const auto& pools = PoolsFor(npc);

        const auto tier = [](RE::FormID loc) -> std::string {
            if (loc == 0) {
                return "(none)";
            }
            const auto& name = GossipGraph::LocationName(loc);
            return name.empty() ? std::format("0x{:08X}", loc) : std::format("{} (0x{:08X})", name, loc);
        };

        std::string rungs;
        float total = 0.0f;
        float reachable = 0.0f;
        for (std::size_t r = 0; r < pools.size(); ++r) {
            const float w = std::max(0.0f, weights[r]);
            const auto& pool = pools[r];
            const auto live = static_cast<std::size_t>(std::count_if(pool.begin(), pool.end(), [](RE::FormID peer) {
                return ActorAvailability(peer) == Availability::Available;
            }));
            total += w;
            if (live > 0) {
                reachable += w;
            }
            rungs +=
                std::format("{}{}={}/{}", rungs.empty() ? "" : " ", RungName(static_cast<int>(r)), live, pool.size());
        }
        // The province rung is always populated, so it is reported but left
        // out of the ratio for the same reason the stall test leaves it out:
        // counting it would mean nobody is ever isolated.
        return std::format("isolation: {} —— household={} settlement={} hold={} | rungs {} | "
                           "reachable share of local weight {:.1f}%",
                           GossipGraph::NpcName(npc),
                           self ? tier(self->household) : "(not a participant)",
                           self ? tier(self->settlement) : "(not a participant)",
                           self ? tier(self->hold) : "(not a participant)",
                           rungs,
                           total > 0.0f ? reachable / total * 100.0f : 0.0f);
    }

    std::vector<RumorView> GetRumorViews(const GossipState& st)
    {
        std::vector<RumorView> out;
        out.reserve(st.rumors.size());

        for (const auto& [id, r] : st.rumors) {
            RumorView v;
            v.id = id;
            v.bands = r.bands;
            v.text = r.bands.empty() ? std::string{} : r.bands.front();
            v.live = r.live;
            v.stalled = IsStalled(r);
            v.notability = r.notability;
            v.ageDays = std::max(0.0, st.simGameDay - r.seedGameDay);
            v.idleDays = std::max(0.0, st.simGameDay - r.lastActivityGameDay);
            v.carriers = r.carriers.size();
            v.maxDepth = r.maxDepth;
            v.transmissions = r.transmissions;
            v.wasted = r.wasted;
            v.originNpc = r.originNpc;
            v.sourceMemoryId = r.sourceMemoryId;

            std::unordered_set<RE::FormID> holds;
            std::unordered_set<RE::FormID> settlements;
            for (const auto& [npc, carrier] : r.carriers) {
                if (!carrier.recovered) {
                    ++v.activeCarriers;
                }
                if (const auto* p = GossipGraph::Find(npc)) {
                    if (p->hold) {
                        holds.insert(p->hold);
                    }
                    if (p->settlement) {
                        settlements.insert(p->settlement);
                    }
                }
            }
            v.holds = holds.size();
            v.settlements = settlements.size();

            v.originName = GossipGraph::NpcName(r.originNpc);
            v.originLocation = GossipGraph::LocationName(r.originSettlement);
            out.push_back(std::move(v));
        }

        // Newest first. Ties broken by id descending so the order is stable
        // across pushes — two rumors seeded in the same poll share a
        // seedGameDay exactly, and an unstable sort would let them swap
        // places between frames.
        std::sort(out.begin(), out.end(), [](const RumorView& a, const RumorView& b) {
            if (a.ageDays != b.ageDays) {
                return a.ageDays < b.ageDays;
            }
            return a.id > b.id;
        });
        return out;
    }

    Stats GetStats(const GossipState& st)
    {
        Stats out;
        out.transmissionsThisSession = st.counters.transmissions;
        out.wastedThisSession = st.counters.wasted;
        out.notCaughtThisSession = st.counters.notCaught;
        out.unavailableThisSession = st.counters.unavailable;
        out.cappedThisSession = st.counters.capped;
        out.memoriesWritten = st.counters.memoriesWritten;
        out.memoryWriteFailures = st.counters.memoryWriteFailures;
        out.liveRumors = static_cast<std::size_t>(
            std::count_if(st.rumors.begin(), st.rumors.end(), [](const auto& kv) { return kv.second.live; }));
        out.totalCarriers = 0;
        for (const auto& [id, r] : st.rumors) {
            out.totalCarriers += r.carriers.size();
        }
        out.queuedEvents = st.queue.size();
        return out;
    }

    void OnSave(SKSE::SerializationInterface* intfc, const GossipState& state)
    {
        if (!intfc) {
            return;
        }
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("GossipSim::OnSave: OpenRecord failed");
            return;
        }

        intfc->WriteRecordData(state.nextRumorId);
        intfc->WriteRecordData(state.simGameDay);

        // Bucket selection history. The count it was drawn against goes
        // first so the load can decide whether the indices still mean
        // anything before it reads them.
        intfc->WriteRecordData(state.bucketCount);
        const auto historyCount = static_cast<std::uint32_t>(state.bucketHistory.size());
        intfc->WriteRecordData(historyCount);
        for (const auto b : state.bucketHistory) {
            intfc->WriteRecordData(b);
        }

        // Only live rumors are persisted. The poll sweep normally clears dead
        // ones already, but a save landing between a burnout and the next
        // sweep would otherwise write a rumor that will be discarded on load
        // anyway. Belt and braces on the payload size.
        const auto rumorCount = static_cast<std::uint32_t>(
            std::count_if(state.rumors.begin(), state.rumors.end(), [](const auto& kv) { return kv.second.live; }));
        intfc->WriteRecordData(rumorCount);
        for (const auto& [id, r] : state.rumors) {
            if (!r.live) {
                continue;
            }
            intfc->WriteRecordData(r.id);
            intfc->WriteRecordData(r.originNpc);
            intfc->WriteRecordData(r.originSettlement);
            intfc->WriteRecordData(r.seedGameDay);
            intfc->WriteRecordData(r.notability);
            intfc->WriteRecordData(r.maxDepth);
            const auto tx = static_cast<std::uint32_t>(r.transmissions);
            const auto wasted = static_cast<std::uint32_t>(r.wasted);
            intfc->WriteRecordData(tx);
            intfc->WriteRecordData(wasted);
            const auto conversations = static_cast<std::uint32_t>(r.conversations);
            const auto notCaught = static_cast<std::uint32_t>(r.notCaught);
            const auto unavailable = static_cast<std::uint32_t>(r.unavailable);
            const auto capped = static_cast<std::uint32_t>(r.capped);
            const auto silent = static_cast<std::uint32_t>(r.silent);
            intfc->WriteRecordData(conversations);
            intfc->WriteRecordData(notCaught);
            intfc->WriteRecordData(unavailable);
            intfc->WriteRecordData(capped);
            intfc->WriteRecordData(silent);
            intfc->WriteRecordData(r.lastActivityGameDay);
            const std::uint8_t live = r.live ? 1 : 0;
            intfc->WriteRecordData(live);
            intfc->WriteRecordData(r.sourceMemoryId);
            intfc->WriteRecordData(r.sourceActor);
            const auto bandCount = static_cast<std::uint32_t>(r.bands.size());
            intfc->WriteRecordData(bandCount);
            for (const auto& b : r.bands) {
                EventLogUtil::WriteString(intfc, b);
            }

            const auto carrierCount = static_cast<std::uint32_t>(r.carriers.size());
            intfc->WriteRecordData(carrierCount);
            for (const auto& [npc, c] : r.carriers) {
                intfc->WriteRecordData(npc);
                intfc->WriteRecordData(c.generation);
                intfc->WriteRecordData(c.toldBy);
                intfc->WriteRecordData(c.heardOnGameDay);
                intfc->WriteRecordData(c.infectiousUntilGameDay);
                intfc->WriteRecordData(c.nextStepGameDay);
                const std::uint8_t recovered = c.recovered ? 1 : 0;
                intfc->WriteRecordData(recovered);
            }
        }
        logger::debug("GossipSim::OnSave: wrote {} live rumors ({} in memory)", rumorCount, state.rumors.size());
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t)
    {
        if (!intfc) {
            return;
        }
        if (version != kRecordVersion) {
            logger::warn(
                "GossipSim::OnLoad: unrecognized record version {} (expected {}); skipping", version, kRecordVersion);
            OnRevert();
            return;
        }

        std::scoped_lock pendingLock(g_pendingMutex);
        if (!g_pending) {
            g_pending.emplace();
        }
        auto& g_rumors = g_pending->rumors;
        auto& g_queue = g_pending->queue;
        auto& g_nextRumorId = g_pending->nextRumorId;
        auto& g_simGameDay = g_pending->simGameDay;
        g_rumors.clear();
        g_queue = {};
        g_pending->bucketHistory.clear();
        g_pending->bucketCount = 0;

        std::uint32_t savedBucketCount = 0;
        std::uint32_t historyCount = 0;
        std::uint32_t rumorCount = 0;
        if (intfc->ReadRecordData(g_nextRumorId) != sizeof(g_nextRumorId)
            || intfc->ReadRecordData(g_simGameDay) != sizeof(g_simGameDay)
            || intfc->ReadRecordData(savedBucketCount) != sizeof(savedBucketCount)
            || intfc->ReadRecordData(historyCount) != sizeof(historyCount)) {
            logger::error("GossipSim::OnLoad: short read on header; reverting");
            g_rumors.clear();
            g_queue = {};
            return;
        }

        // Read the history whatever happens — the bytes are in the stream
        // and the rumor count comes after them, so skipping the read
        // would desynchronise every field that follows. Whether to KEEP
        // it is a separate question, decided below.
        std::deque<std::uint32_t> history;
        for (std::uint32_t i = 0; i < historyCount; ++i) {
            std::uint32_t b = 0;
            if (intfc->ReadRecordData(b) != sizeof(b)) {
                logger::error("GossipSim::OnLoad: short read on bucket history; reverting");
                g_rumors.clear();
                g_queue = {};
                return;
            }
            history.push_back(b);
        }

        const auto liveBucketCount = GossipGraph::BucketCount();
        if (savedBucketCount == liveBucketCount) {
            g_pending->bucketHistory = std::move(history);
            g_pending->bucketCount = savedBucketCount;
        } else if (savedBucketCount != 0) {
            // Every participant has been reassigned, so bucket 3 in the
            // save and bucket 3 now are different sets of people.
            // Excluding the old indices would exclude an arbitrary group.
            // Logged rather than silent: one unusually clustered cycle
            // after a settings change should have an explanation sitting
            // in the log when somebody notices it.
            logger::info("GossipSim::OnLoad: iGossipHarvestBuckets changed ({} -> {}); discarding {} "
                         "remembered bucket selection(s)",
                         savedBucketCount,
                         liveBucketCount,
                         history.size());
        }

        if (intfc->ReadRecordData(rumorCount) != sizeof(rumorCount)) {
            logger::error("GossipSim::OnLoad: short read on rumor count; reverting");
            g_rumors.clear();
            g_queue = {};
            return;
        }

        for (std::uint32_t i = 0; i < rumorCount; ++i) {
            Rumor r;
            std::uint32_t tx = 0;
            std::uint32_t wasted = 0;
            std::uint32_t conversations = 0;
            std::uint32_t notCaught = 0;
            std::uint32_t unavailable = 0;
            std::uint32_t capped = 0;
            std::uint32_t silent = 0;
            std::uint8_t live = 0;
            if (intfc->ReadRecordData(r.id) != sizeof(r.id) || intfc->ReadRecordData(r.originNpc) != sizeof(r.originNpc)
                || intfc->ReadRecordData(r.originSettlement) != sizeof(r.originSettlement)
                || intfc->ReadRecordData(r.seedGameDay) != sizeof(r.seedGameDay)
                || intfc->ReadRecordData(r.notability) != sizeof(r.notability)
                || intfc->ReadRecordData(r.maxDepth) != sizeof(r.maxDepth) || intfc->ReadRecordData(tx) != sizeof(tx)
                || intfc->ReadRecordData(wasted) != sizeof(wasted)
                || intfc->ReadRecordData(conversations) != sizeof(conversations)
                || intfc->ReadRecordData(notCaught) != sizeof(notCaught)
                || intfc->ReadRecordData(unavailable) != sizeof(unavailable)
                || intfc->ReadRecordData(capped) != sizeof(capped) || intfc->ReadRecordData(silent) != sizeof(silent)
                || intfc->ReadRecordData(r.lastActivityGameDay) != sizeof(r.lastActivityGameDay)
                || intfc->ReadRecordData(live) != sizeof(live)) {
                logger::error("GossipSim::OnLoad: short read on rumor {}; reverting", i);
                g_rumors.clear();
                g_queue = {};
                return;
            }
            std::uint32_t bandCount = 0;
            if (intfc->ReadRecordData(r.sourceMemoryId) != sizeof(r.sourceMemoryId)
                || intfc->ReadRecordData(r.sourceActor) != sizeof(r.sourceActor)
                || intfc->ReadRecordData(bandCount) != sizeof(bandCount)) {
                logger::error("GossipSim::OnLoad: short read on rumor provenance; reverting");
                g_rumors.clear();
                g_queue = {};
                return;
            }
            r.bands.resize(bandCount);
            for (auto& b : r.bands) {
                if (!EventLogUtil::ReadString(intfc, b)) {
                    logger::error("GossipSim::OnLoad: short read on band text; reverting");
                    g_rumors.clear();
                    g_queue = {};
                    return;
                }
            }
            r.transmissions = tx;
            r.wasted = wasted;
            r.conversations = conversations;
            r.notCaught = notCaught;
            r.unavailable = unavailable;
            r.capped = capped;
            r.silent = silent;
            r.live = live != 0;

            std::uint32_t carrierCount = 0;
            if (intfc->ReadRecordData(carrierCount) != sizeof(carrierCount)) {
                logger::error("GossipSim::OnLoad: short read on carrier count; reverting");
                g_rumors.clear();
                g_queue = {};
                return;
            }
            for (std::uint32_t c = 0; c < carrierCount; ++c) {
                RE::FormID npc = 0;
                Carrier carrier;
                std::uint8_t recovered = 0;
                if (intfc->ReadRecordData(npc) != sizeof(npc)
                    || intfc->ReadRecordData(carrier.generation) != sizeof(carrier.generation)
                    || intfc->ReadRecordData(carrier.toldBy) != sizeof(carrier.toldBy)
                    || intfc->ReadRecordData(carrier.heardOnGameDay) != sizeof(carrier.heardOnGameDay)
                    || intfc->ReadRecordData(carrier.infectiousUntilGameDay) != sizeof(carrier.infectiousUntilGameDay)
                    || intfc->ReadRecordData(carrier.nextStepGameDay) != sizeof(carrier.nextStepGameDay)
                    || intfc->ReadRecordData(recovered) != sizeof(recovered)) {
                    logger::error("GossipSim::OnLoad: short read on carrier; reverting");
                    g_rumors.clear();
                    g_queue = {};
                    return;
                }
                carrier.recovered = recovered != 0;

                // FormIDs must be resolved through the load order the
                // save was made with.
                RE::FormID resolved = 0;
                if (!intfc->ResolveFormID(npc, resolved)) {
                    continue;
                }
                if (!carrier.recovered) {
                    ScheduleLocked(r.id, resolved, carrier.nextStepGameDay);
                }
                r.carriers.emplace(resolved, carrier);
            }

            RE::FormID resolvedOrigin = 0;
            if (intfc->ResolveFormID(r.originNpc, resolvedOrigin)) {
                r.originNpc = resolvedOrigin;
            }
            g_rumors.emplace(r.id, std::move(r));
        }

        logger::info("GossipSim::OnLoad: restored {} rumors, {} queued events", g_rumors.size(), g_queue.size());
        PublishSnapshot();
    }

    void OnRevert()
    {
        // Clears only the SIMULATION's portion of the pending state.
        // GossipClaims::OnRevert clears the ledger's. Keeping them
        // separate means the order SKSE dispatches the two records in
        // cannot matter, which it otherwise would: a revert that wiped
        // the whole staging area could erase what the other module's
        // OnLoad had already written into it.
        {
            std::scoped_lock pendingLock(g_pendingMutex);
            if (!g_pending) {
                g_pending.emplace();
            }
            g_pending->rumors.clear();
            g_pending->queue = {};
            g_pending->nextRumorId = 1;
            g_pending->lastGameDaySample = -1.0;
            g_pending->simGameDay = 0.0;
            g_pending->counters = {};
            g_pending->harvest = {};
            g_pending->bucketHistory.clear();
            g_pending->bucketCount = 0;
        }
        // The contact cache is derived rather than owned, so it is not in
        // the pending state. AdoptPendingState clears it on the gossip
        // thread, which is the only thread allowed to touch it — dropping
        // it from here would be a cross-thread write to live state.
    }
} // namespace NarrativeEngine::GossipSim
