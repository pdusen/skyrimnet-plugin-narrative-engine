#include <GossipSim.h>

#include <GossipDispatch.h>

#include <optional>

#include <GossipState.h>

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
#include <deque>
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
        // v6 adds the harvest bucket selection history.
        constexpr std::uint32_t kRecordVersion = 6;

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
            if (total <= 0.0f) {
                return g_contactCache.emplace(npc, std::move(out)).first->second;
            }

            // The province channel is a SHARE OF THE BUDGET, not another
            // weight thrown into the same division.
            //
            // As a weight it was unusable, because it competed against a
            // sum this carrier happens to own. One housemate contributes
            // 600; a College mage with four of them and a settlement full
            // of neighbours carries a named total near 2500, while a
            // crofter with two neighbours carries about 2. The same
            // province weight therefore bought wildly different odds
            // depending on how well connected somebody was — and worst
            // odds for exactly the well-connected carriers who actually
            // spread things. Measured over a full run: 203 transmissions,
            // 891 conversations, not one province draw, and none possible
            // in any realistic session.
            //
            // As a share it means one plain thing that holds for everyone:
            // this fraction of a carrier's conversations are with somebody
            // from anywhere in Skyrim. The named contacts divide the rest
            // among themselves exactly as before, so the daily budget
            // still sums to fGossipConversationsPerDay.
            //
            // Population is deliberately NOT a factor. It was, and that
            // was double-counting: a carrier's conversations are a fixed
            // daily budget, so how many people exist elsewhere changes who
            // the stranger turns out to be, never how often they meet one.
            const float share = std::clamp(cfg.gossipProvinceShare, 0.0f, 0.5f);
            const float budget = std::max(0.1f, cfg.gossipConversationsPerDay);
            out.reserve(weighted.size() + 1);
            for (auto& [peer, c] : weighted) {
                Contact copy = c;
                copy.rate = budget * (1.0f - share) * c.rate / total;
                out.push_back(copy);
            }
            if (share > 0.0f) {
                out.push_back(
                    {kProvincePeer, budget * share, GossipGraph::Channel::Province, GossipGraph::Channel::Province, 0});
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
                ++g_counters.memoryWriteFailures;
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
                    ++g_counters.memoriesWritten;
                } else {
                    ++g_counters.memoryWriteFailures;
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
            stats.conversations = rumor.conversations;
            stats.notCaught = rumor.notCaught;
            stats.unavailable = rumor.unavailable;
            stats.capped = rumor.capped;
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
            rumor.conversations += static_cast<std::size_t>(std::max(0, conversations));

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
                        ++rumor.unavailable;
                        ++g_counters.unavailable;
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
        bool IsStalled(const Rumor& rumor)
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
            g_contactCache.clear();
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
                                   + g_counters.unavailable + g_counters.capped;
        GossipLog::Note(std::format("CENSUS  live rumors={}  conversations={} ({} told, {} knew, {} missed, "
                                    "{} away, {} capped)  memories={} (failed {})",
                                    g_rumors.size(),
                                    conversations,
                                    g_counters.transmissions,
                                    g_counters.wasted,
                                    g_counters.notCaught,
                                    g_counters.unavailable,
                                    g_counters.capped,
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
        float total = 0.0f;
        float reachable = 0.0f;
        for (const auto& c : ContactsFor(npc)) {
            if (c.peer == kProvincePeer || c.rate <= 0.0f) {
                continue;
            }
            total += c.rate;
            if (ActorAvailability(c.peer) == Availability::Available) {
                reachable += c.rate;
            }
        }
        return total > 0.0f ? reachable / total : 0.0f;
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
            intfc->WriteRecordData(conversations);
            intfc->WriteRecordData(notCaught);
            intfc->WriteRecordData(unavailable);
            intfc->WriteRecordData(capped);
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
                || intfc->ReadRecordData(capped) != sizeof(capped)
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
