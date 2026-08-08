#include <GossipSim.h>

#include <EventLogUtil.h>
#include <GossipClaims.h>
#include <GossipContent.h>
#include <GossipGraph.h>
#include <GossipLog.h>
#include <logger.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <format>
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
        constexpr std::uint32_t kRecordVersion = 4;

        // Sentinel peer standing in for "somebody, anywhere in Skyrim".
        // Resolved to a random participant only if it is actually
        // selected — materialising ~850 province edges per carrier to
        // model a 0.0001-weight channel would be absurd.
        constexpr RE::FormID kProvincePeer = 0xFFFFFFFF;

        // Clamp on the game-time delta credited in one poll. A delta
        // larger than this is almost certainly a console `set timescale`
        // experiment or a debug time jump rather than play; crediting it
        // in full would schedule an unbounded burst.
        constexpr double kMaxGameDayDeltaPerPoll = 3.0;

        // Carrier state under SIR. A carrier is Infectious from
        // `heardOnGameDay` until `infectiousUntilGameDay`, then Recovered —
        // permanently immune and never re-infectable.
        //
        // Note what is NOT here: no per-carrier notability, no telling quota,
        // no household-saturation flag. Transmissibility is constant and lives
        // on the rumor; nothing about a carrier depletes. Three earlier models
        // failed by making spread a function of how far a rumor had already
        // travelled, and every one of those fields was part of that mistake.
        struct Carrier
        {
            std::uint32_t generation = 0;
            RE::FormID toldBy = 0;
            double heardOnGameDay = 0.0;
            double infectiousUntilGameDay = 0.0;
            double nextStepGameDay = 0.0;
            bool recovered = false;
        };

        struct Rumor
        {
            std::uint32_t id = 0;
            RE::FormID originNpc = 0;
            RE::FormID originSettlement = 0;
            double seedGameDay = 0.0;
            // Constant for the rumor's whole life. Per-conversation
            // transmission probability is `notability * transmissionScale`.
            float notability = 1.0f;
            // Provenance. Without a recorded source, "no memory is ever
            // used twice" cannot be verified from the trace — a claimed
            // memory and the rumor it produced would only be correlated
            // by timing.
            std::int64_t sourceMemoryId = 0;
            RE::FormID sourceActor = 0;
            // Generation-banded text, all produced by one call at seed
            // time. Selected by the receiving carrier's generation.
            std::vector<std::string> bands;
            // Every NPC that has EVER carried this rumor. Membership here is
            // what makes someone immune, so it must never be pruned while the
            // rumor is live — a removed entry would be re-infectable and the
            // outbreak would never terminate.
            std::unordered_map<RE::FormID, Carrier> carriers;
            std::uint32_t maxDepth = 0;
            std::size_t transmissions = 0;
            std::size_t wasted = 0;
            double lastActivityGameDay = 0.0;
            bool live = true;
        };

        struct QueueEntry
        {
            double dueGameDay = 0.0;
            std::uint32_t rumorId = 0;
            RE::FormID carrier = 0;

            // std::priority_queue is a max-heap; invert so the earliest
            // due event pops first.
            bool operator<(const QueueEntry& other) const
            {
                return dueGameDay > other.dueGameDay;
            }
        };

        std::mutex g_mutex;
        std::unordered_map<std::uint32_t, Rumor> g_rumors;
        std::priority_queue<QueueEntry> g_queue;
        std::uint32_t g_nextRumorId = 1;

        double g_lastGameDaySample = -1.0;
        double g_simGameDay = 0.0;
        double g_secondsSinceTick = 0.0;

        Stats g_stats;
        std::mt19937 g_rng{1337};

        // Per-carrier contact set, memoised for the session. The graph
        // is immutable between session starts, so a carrier's weighted
        // contact list never changes and recomputing it per event would
        // be pure waste.
        struct Contact
        {
            RE::FormID peer = 0;
            float rate = 0.0f; // contacts per in-world day
            // The specific channel that best EXPLAINS this contact
            // (relationship / faction where one exists, else the tier).
            GossipGraph::Channel via = GossipGraph::Channel::Settlement;
            // The proximity tier the pair share, recorded separately.
            //
            // Collapsing these into one field made the logged channel mix
            // unreadable: 371 of 636 vanilla relationship edges are
            // same-household, so most physically-household conversations were
            // labelled "relationship" and household looked like 15% of the
            // traffic when it is in fact the dominant channel.
            GossipGraph::Channel tier = GossipGraph::Channel::Settlement;
            RE::FormID faction = 0;
        };
        std::unordered_map<RE::FormID, std::vector<Contact>> g_contactCache;

        double NowGameDay()
        {
            return EventLogUtil::NowGameTimeSeconds() / 86400.0;
        }

        float UniformFloat(float lo, float hi)
        {
            std::uniform_real_distribution<float> d(lo, hi);
            return d(g_rng);
        }

        // Knuth's method. lambda is small here — a fraction of a conversation
        // per simulation step — so the loop runs a couple of times at most.
        int DrawPoisson(double lambda)
        {
            if (lambda <= 0.0) {
                return 0;
            }
            const double limit = std::exp(-lambda);
            double product = 1.0;
            int n = 0;
            std::uniform_real_distribution<double> uni(0.0, 1.0);
            while (n < 60) {
                product *= uni(g_rng);
                if (product <= limit) {
                    break;
                }
                ++n;
            }
            return n;
        }

        // Build the weighted contact list for one carrier and divide the
        // daily conversation budget among it.
        //
        // The division is the whole point: weights are RATIOS. A
        // 90-resident city and a two-person farmstead both yield roughly
        // fGossipConversationsPerDay contacts per day; the city dweller
        // simply spreads theirs more thinly.
        const std::vector<Contact>& ContactsFor(RE::FormID npc)
        {
            if (const auto it = g_contactCache.find(npc); it != g_contactCache.end()) {
                return it->second;
            }

            const auto& cfg = Settings::Get();
            std::vector<Contact> out;
            std::unordered_map<RE::FormID, Contact> weighted;

            const auto* self = GossipGraph::Find(npc);
            if (!self) {
                return g_contactCache.emplace(npc, std::move(out)).first->second;
            }

            // `specific` marks a channel that EXPLAINS a contact the
            // proximity tiers could not have produced. When a peer is
            // reachable both ways — a housemate who is also your sister,
            // a settlement neighbour who is also a guild-mate — the
            // specific channel wins the attribution, because that is the
            // more informative answer in the log. It matters most for
            // cross-hold transmissions, where only a personal edge or
            // the province channel can have enabled the contact at all.
            const auto add =
                [&](RE::FormID peer, float weight, GossipGraph::Channel via, RE::FormID faction, bool specific) {
                    if (peer == npc || weight <= 0.0f) {
                        return;
                    }
                    auto& c = weighted[peer];
                    const bool firstTouch = c.peer == 0;
                    if (firstTouch) {
                        c.peer = peer;
                        // Proximity tiers are added closest-first, so whichever
                        // touches this peer first is the tightest they share.
                        c.tier = via;
                    }
                    if (firstTouch || specific) {
                        c.via = via;
                        c.faction = faction;
                    }
                    c.rate += weight;
                };

            if (self->household) {
                for (const auto peer : GossipGraph::HouseholdMembers(self->household)) {
                    add(peer, cfg.gossipWeightHousehold, GossipGraph::Channel::Household, 0, false);
                }
            }
            if (self->settlement) {
                for (const auto peer : GossipGraph::SettlementMembers(self->settlement)) {
                    add(peer, cfg.gossipWeightSettlement, GossipGraph::Channel::Settlement, 0, false);
                }
            }
            if (self->hold) {
                for (const auto peer : GossipGraph::HoldMembers(self->hold)) {
                    add(peer, cfg.gossipWeightHold, GossipGraph::Channel::Hold, 0, false);
                }
            }
            for (const auto& edge : GossipGraph::PersonalEdges(npc)) {
                // Distance attenuation. A guild-mate in your own settlement is
                // someone you see constantly; one three holds away you see
                // rarely. Without this the channel is the dominant cross-hold
                // leak at weight 40.
                const auto* other = GossipGraph::Find(edge.other);
                float distance = cfg.gossipPersonalDistanceFar;
                if (other) {
                    if (self->settlement != 0 && other->settlement == self->settlement) {
                        distance = cfg.gossipPersonalDistanceSameSettlement;
                    } else if (other->hold == self->hold) {
                        distance = cfg.gossipPersonalDistanceSameHold;
                    }
                }
                add(edge.other, cfg.gossipWeightPersonalEdge * distance, edge.via, edge.faction, true);
            }

            // The relationship multiplier scales a peer's WHOLE weight,
            // applied exactly once after accumulation. You talk to a
            // sibling in the same house more than to a housemate you
            // merely tolerate, and to an Archnemesis not at all.
            //
            // Folding it into the personal-edge term above instead would
            // apply it twice for any pair that is both a neighbour and a
            // relation — which, given that 92% of vanilla relationship
            // edges are same-household or same-settlement, is very nearly
            // all of them.
            for (const auto& edge : GossipGraph::PersonalEdges(npc)) {
                if (const auto it = weighted.find(edge.other); it != weighted.end()) {
                    it->second.rate *= edge.multiplier;
                }
            }

            float total = 0.0f;
            for (const auto& [peer, c] : weighted) {
                total += c.rate;
            }
            const float provinceWeight = cfg.gossipWeightProvince * static_cast<float>(GossipGraph::ParticipantCount());
            total += provinceWeight;
            if (total <= 0.0f) {
                return g_contactCache.emplace(npc, std::move(out)).first->second;
            }

            const float budget = std::max(0.1f, cfg.gossipConversationsPerDay);
            out.reserve(weighted.size() + 1);
            for (auto& [peer, c] : weighted) {
                Contact copy = c;
                copy.rate = budget * c.rate / total;
                out.push_back(copy);
            }
            if (provinceWeight > 0.0f) {
                out.push_back({kProvincePeer,
                               budget * provinceWeight / total,
                               GossipGraph::Channel::Province,
                               GossipGraph::Channel::Province,
                               0});
            }
            return g_contactCache.emplace(npc, std::move(out)).first->second;
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

        // The two memories a transmission writes. No LLM here — this is a
        // string build from band text the seed-time call already produced,
        // plus relationship-aware framing.
        //
        // The Milestone 1 [NE-GOSSIP-STUB ...] prefix and the "stub" tag are
        // gone; the "gossip" tag stays. Type stays KNOWLEDGE, which is what
        // keeps gossip's own output out of the harvester's candidate set.
        //
        // `teller` and `listener` arrive as TESNPC BASE forms, because that
        // is what the carrier map and the whole graph are keyed on. Every
        // id handed to SkyrimNet — the memory owner and the related-actor
        // array alike — must be the PLACED REFERENCE instead. These are
        // different FormIDs for the same person, and writing a memory
        // against the base form addresses nobody.
        void WriteMemories(const Rumor& rumor,
                           RE::FormID teller,
                           RE::FormID listener,
                           std::uint32_t generation,
                           RE::FormID location)
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
                ++g_stats.memoryWriteFailures;
                if (Settings::Get().debugMode) {
                    logger::debug("GossipSim: no placed reference for {} -> {}; memory pair skipped",
                                  tellerRef == 0 ? GossipGraph::NpcName(teller) : GossipGraph::NpcName(listener),
                                  rumor.id);
                }
                return;
            }

            const auto band = std::min(GossipContent::BandForGeneration(generation), rumor.bands.size() - 1);
            const auto composed = GossipContent::Compose(rumor.bands[band], teller, listener);

            const auto& locName = GossipGraph::LocationName(location);
            const auto tags = std::string{R"(["gossip"])"};
            const int a = SkyrimNetAPI::AddMemory(tellerRef,
                                                  composed.tellerText,
                                                  rumor.notability,
                                                  "KNOWLEDGE",
                                                  "",
                                                  locName,
                                                  tags,
                                                  std::format("[{}]", listenerRef));
            const int b = SkyrimNetAPI::AddMemory(listenerRef,
                                                  composed.listenerText,
                                                  rumor.notability,
                                                  "KNOWLEDGE",
                                                  "",
                                                  locName,
                                                  tags,
                                                  std::format("[{}]", tellerRef));
            for (const int id : {a, b}) {
                if (id > 0) {
                    ++g_stats.memoriesWritten;
                } else {
                    ++g_stats.memoryWriteFailures;
                }
            }
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

            const auto& contacts = ContactsFor(carrierId);
            float totalRate = 0.0f;
            for (const auto& c : contacts) {
                totalRate += c.rate;
            }
            if (contacts.empty() || totalRate <= 0.0f) {
                recover("no-contacts");
                return;
            }

            const double step = std::max(0.01f, cfg.gossipStepDays);
            const double beta =
                std::clamp(static_cast<double>(rumor.notability * cfg.gossipTransmissionScale), 0.0, 1.0);
            const int conversations = DrawPoisson(std::max(0.0f, cfg.gossipConversationsPerDay) * step);

            std::uniform_real_distribution<float> pick(0.0f, totalRate);
            std::uniform_real_distribution<double> roll01(0.0, 1.0);
            const auto* fromP = GossipGraph::Find(carrierId);

            for (int i = 0; i < conversations; ++i) {
                float roll = pick(g_rng);
                const Contact* chosen = &contacts.back();
                for (const auto& c : contacts) {
                    roll -= c.rate;
                    if (roll <= 0.0f) {
                        chosen = &c;
                        break;
                    }
                }

                RE::FormID listener = chosen->peer;
                auto via = chosen->via;
                auto tier = chosen->tier;
                RE::FormID viaFaction = chosen->faction;
                if (listener == kProvincePeer) {
                    const auto& all = GossipGraph::Participants();
                    if (all.empty()) {
                        continue;
                    }
                    std::uniform_int_distribution<std::size_t> d(0, all.size() - 1);
                    listener = all[d(g_rng)];
                    via = GossipGraph::Channel::Province;
                    tier = GossipGraph::Channel::Province;
                    viaFaction = 0;
                }

                if (listener == carrierId || rumor.carriers.contains(listener)) {
                    // Already infectious, or recovered and immune. A wasted
                    // opportunity — the brake.
                    ++rumor.wasted;
                    ++g_stats.wastedThisSession;
                    GossipLog::Wasted(rumorId, carrierId, listener, conversations - i - 1);
                    continue;
                }

                if (roll01(g_rng) >= beta) {
                    continue; // they spoke; it did not catch
                }

                const auto* toP = GossipGraph::Find(listener);
                if (!toP || ActorAvailability(listener) != Availability::Available) {
                    // No conversation happened. Deliberately NOT counted as a
                    // wasted telling: wasted tellings are the saturation brake
                    // and mean "they already knew", which is a different
                    // thing. Miscounting here would make the outbreak look
                    // more saturated than it is and terminate early. The
                    // listener stays susceptible and can catch it later.
                    continue;
                }
                if (static_cast<int>(rumor.carriers.size()) >= cfg.gossipMaxCarriersPerRumor) {
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
                ++g_stats.transmissionsThisSession;
                rumor.lastActivityGameDay = nowGameDay;

                const RE::FormID location = toP->settlement ? toP->settlement : toP->hold;
                GossipLog::Tell(rumorId,
                                fresh.generation,
                                rumor.notability,
                                carrierId,
                                listener,
                                via,
                                tier,
                                viaFaction,
                                location,
                                fromP ? fromP->hold : 0,
                                toP->hold);

                WriteMemories(rumor, carrierId, listener, fresh.generation, location);
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
        void SweepAndReapLocked()
        {
            // Claim expiry rides the same sampled game time as the rumor reap.
            // Deliberately not gated on there being live rumors: a quiet
            // stretch still has to let claims age out, or a lull would freeze
            // the ledger.
            if (const auto expired = GossipClaims::Sweep(g_simGameDay); expired > 0) {
                logger::debug("GossipClaims: expired {} claim(s); {} memory + {} event claim(s) remain",
                              expired,
                              GossipClaims::Count(),
                              GossipClaims::EventCount());
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
        // True when every still-infectious carrier finds all of their NAMED
        // contacts already carrying it. The rumor is alive — carriers are
        // still scheduled, still burning down their infectious window — but
        // there is nobody left for them to reach.
        //
        // kProvincePeer is skipped on purpose. It is a sentinel standing for
        // "somebody, anywhere in Skyrim", resolved to a random participant at
        // transmission time, and every carrier holds one. Counting it as a
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
        bool IsStalledLocked(const Rumor& rumor)
        {
            bool anyActive = false;
            for (const auto& [npc, carrier] : rumor.carriers) {
                if (carrier.recovered) {
                    continue;
                }
                anyActive = true;
                for (const auto& c : ContactsFor(npc)) {
                    if (c.peer == kProvincePeer || c.rate <= 0.0f) {
                        continue;
                    }
                    if (!rumor.carriers.contains(c.peer)) {
                        return false; // somebody left to tell
                    }
                }
            }
            // No infectious carriers at all means it is not going anywhere
            // either — though the reap normally removes such a rumor in the
            // same poll that produced it.
            (void)anyActive;
            return true;
        }
    } // namespace

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

        std::scoped_lock lock(g_mutex);
        g_contactCache.clear();
        // Re-base the game-time sample so a load does not read as a
        // multi-year jump on the first poll.
        g_lastGameDaySample = NowGameDay();
        g_simGameDay = g_lastGameDaySample;
        g_secondsSinceTick = 0.0;
        g_stats.transmissionsThisSession = 0;
        g_stats.wastedThisSession = 0;
        g_stats.memoriesWritten = 0;
        g_stats.memoryWriteFailures = 0;
    }

    void OnSessionEnd()
    {
        std::scoped_lock lock(g_mutex);
        if (g_rumors.empty()) {
            return;
        }
        GossipLog::Note(std::format("CENSUS  live rumors={}  transmissions={}  wasted={}  memories={} (failed {})",
                                    g_rumors.size(),
                                    g_stats.transmissionsThisSession,
                                    g_stats.wastedThisSession,
                                    g_stats.memoriesWritten,
                                    g_stats.memoryWriteFailures));
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

    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds)
    {
        const auto& cfg = Settings::Get();
        if (!cfg.gossipEnabled || !GossipGraph::IsReady()) {
            return;
        }

        std::scoped_lock lock(g_mutex);

        g_secondsSinceTick += unpausedElapsedSeconds;
        const double interval = static_cast<double>(std::max(1, cfg.gossipTickIntervalSeconds));
        if (g_secondsSinceTick < interval) {
            return;
        }
        g_secondsSinceTick -= interval;

        // Game time is sampled as a VALUE. Never a timer of our own —
        // TIMESCALE is player-adjustable at runtime and the clock is not
        // pause-aware.
        const double now = NowGameDay();
        if (g_lastGameDaySample < 0.0) {
            g_lastGameDaySample = now;
            g_simGameDay = now;
            return;
        }
        double delta = now - g_lastGameDaySample;
        if (delta < 0.0) {
            // Clock went backwards — a load of an older save, or a
            // console time change. Re-base rather than reason about it.
            g_lastGameDaySample = now;
            g_simGameDay = now;
            return;
        }
        if (delta > kMaxGameDayDeltaPerPoll) {
            // Credit only what we clamped to, and leave the sample
            // BEHIND by the remainder so the rest is credited on
            // subsequent polls.
            //
            // Advancing the sample to `now` here would silently discard
            // the excess, leaving the simulation permanently behind the
            // game clock — every clamped jump would lose real simulated
            // time that never comes back.
            GossipLog::Note(std::format("catch-up: game-time delta {:.2f} days; crediting {:.2f} now, "
                                        "{:.2f} carried forward",
                                        delta,
                                        kMaxGameDayDeltaPerPoll,
                                        delta - kMaxGameDayDeltaPerPoll));
            delta = kMaxGameDayDeltaPerPoll;
        }
        g_lastGameDaySample += delta;
        g_simGameDay += delta;

        // Drain the due queue under BOTH a wall-clock budget and a count
        // cap. Nothing is dropped: whatever is left stays queued and drains
        // on the next firing, so the load spreads across ticks by design.
        //
        // The TIME budget is the real governor. Per-event cost is dominated
        // by the two AddMemory calls a transmission makes, and that cost
        // grows with the size of SkyrimNet's memory database — so bounding
        // time is self-tuning in a way that bounding a count is not. The
        // count is now only a backstop against a pathological schedule.
        //
        // Note an "event" is a carrier-STEP, not a conversation: each one
        // runs Poisson(conversationsPerDay * stepDays) conversations.
        const int cap = std::max(1, cfg.gossipMaxEventsPerTick);
        const auto budgetStart = std::chrono::steady_clock::now();
        const std::chrono::milliseconds budget{std::max(1, cfg.gossipMaxMillisecondsPerTick)};
        int processed = 0;
        bool outOfTime = false;
        while (!g_queue.empty() && g_queue.top().dueGameDay <= g_simGameDay) {
            if (processed >= cap) {
                break;
            }
            // Checked every 4 events rather than every 16. The count cap no
            // longer binds in practice, so this budget is the only thing
            // governing the drain — and each event can make two AddMemory
            // calls into SkyrimNet's vector database, whose cost is not
            // something this side can predict. At a 16-event stride the
            // budget could overshoot by ~32 database writes before noticing.
            if (processed > 0 && (processed % 4) == 0 && std::chrono::steady_clock::now() - budgetStart > budget) {
                outOfTime = true;
                break;
            }
            const auto entry = g_queue.top();
            g_queue.pop();
            // Process AT the event's scheduled time, not at the current
            // simulated time. This is what makes catch-up actually catch
            // up: a carrier due on day 5.2 when the clock has jumped to
            // day 6.0 reschedules from 5.2, is immediately due again,
            // and works through its backlog inside this same drain loop.
            //
            // Using g_simGameDay instead would silently collapse each
            // carrier's backlog to a single event per jump, so a 24-hour
            // wait would advance every rumor by exactly one telling
            // regardless of how much time passed.
            //
            // The loop cannot spin: a carrier reschedules a full
            // gossipStepDays ahead every time, so it can only fire
            // 1/stepDays times per simulated day.
            ProcessEventLocked(entry.rumorId, entry.carrier, entry.dueGameDay);
            ++processed;
        }

        // Only complain when the queue is genuinely backed up — i.e. we
        // stopped while events were still due. Stopping because nothing
        // is due yet is the normal, healthy case and used to be reported
        // as a stall.
        const bool stillDue = !g_queue.empty() && g_queue.top().dueGameDay <= g_simGameDay;
        if (stillDue) {
            GossipLog::Note(std::format("catch-up: processed {} ({}), {} events still due",
                                        processed,
                                        outOfTime ? "time budget" : "work cap",
                                        g_queue.size()));
        }

        SweepAndReapLocked();
    }

    std::uint32_t SeedRumor(RE::FormID originNpc,
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

        std::scoped_lock lock(g_mutex);

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

    std::vector<RumorView> GetRumorViews()
    {
        std::scoped_lock lock(g_mutex);
        std::vector<RumorView> out;
        out.reserve(g_rumors.size());

        for (const auto& [id, r] : g_rumors) {
            RumorView v;
            v.id = id;
            v.bands = r.bands;
            v.text = r.bands.empty() ? std::string{} : r.bands.front();
            v.live = r.live;
            v.stalled = IsStalledLocked(r);
            v.notability = r.notability;
            v.ageDays = std::max(0.0, g_simGameDay - r.seedGameDay);
            v.idleDays = std::max(0.0, g_simGameDay - r.lastActivityGameDay);
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

    Stats GetStats()
    {
        std::scoped_lock lock(g_mutex);
        Stats out = g_stats;
        out.liveRumors = static_cast<std::size_t>(
            std::count_if(g_rumors.begin(), g_rumors.end(), [](const auto& kv) { return kv.second.live; }));
        out.totalCarriers = 0;
        for (const auto& [id, r] : g_rumors) {
            out.totalCarriers += r.carriers.size();
        }
        out.queuedEvents = g_queue.size();
        return out;
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("GossipSim::OnSave: OpenRecord failed");
            return;
        }

        intfc->WriteRecordData(g_nextRumorId);
        intfc->WriteRecordData(g_simGameDay);

        // Only live rumors are persisted. The poll sweep normally clears dead
        // ones already, but a save landing between a burnout and the next
        // sweep would otherwise write a rumor that will be discarded on load
        // anyway. Belt and braces on the payload size.
        const auto rumorCount = static_cast<std::uint32_t>(
            std::count_if(g_rumors.begin(), g_rumors.end(), [](const auto& kv) { return kv.second.live; }));
        intfc->WriteRecordData(rumorCount);
        for (const auto& [id, r] : g_rumors) {
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
        logger::debug("GossipSim::OnSave: wrote {} live rumors ({} in memory)", rumorCount, g_rumors.size());
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

        std::scoped_lock lock(g_mutex);
        g_rumors.clear();
        g_queue = {};

        std::uint32_t rumorCount = 0;
        if (intfc->ReadRecordData(g_nextRumorId) != sizeof(g_nextRumorId)
            || intfc->ReadRecordData(g_simGameDay) != sizeof(g_simGameDay)
            || intfc->ReadRecordData(rumorCount) != sizeof(rumorCount)) {
            logger::error("GossipSim::OnLoad: short read on header; reverting");
            g_rumors.clear();
            g_queue = {};
            return;
        }

        for (std::uint32_t i = 0; i < rumorCount; ++i) {
            Rumor r;
            std::uint32_t tx = 0;
            std::uint32_t wasted = 0;
            std::uint8_t live = 0;
            if (intfc->ReadRecordData(r.id) != sizeof(r.id) || intfc->ReadRecordData(r.originNpc) != sizeof(r.originNpc)
                || intfc->ReadRecordData(r.originSettlement) != sizeof(r.originSettlement)
                || intfc->ReadRecordData(r.seedGameDay) != sizeof(r.seedGameDay)
                || intfc->ReadRecordData(r.notability) != sizeof(r.notability)
                || intfc->ReadRecordData(r.maxDepth) != sizeof(r.maxDepth) || intfc->ReadRecordData(tx) != sizeof(tx)
                || intfc->ReadRecordData(wasted) != sizeof(wasted)
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
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        g_rumors.clear();
        g_queue = {};
        g_contactCache.clear();
        g_nextRumorId = 1;
        g_lastGameDaySample = -1.0;
        g_simGameDay = 0.0;
        g_secondsSinceTick = 0.0;
        g_stats = {};
    }
} // namespace NarrativeEngine::GossipSim
