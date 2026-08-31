#include <PlotPopulation.h>

#include <GossipGraph.h>
#include <logger.h>
#include <PlotFactionRoster.h>
#include <Settings.h>
#include <TravelGraph.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NarrativeEngine::PlotPopulation
{
    namespace
    {
        PlotCasting::Population g_population;
        bool g_ready = false;

        // --- The road-distance table ------------------------------------
        //
        // Distinct road nodes the population actually occupies, and the
        // normalised distance between every pair of them. Dense rather
        // than sparse because there are as many slots as there are
        // inhabited settlements -- around sixty in vanilla -- so the whole
        // matrix is a few thousand floats.
        //
        // Built here rather than queried per dispatch for two reasons: a
        // Dijkstra per step would be wasted work on an answer that cannot
        // change, and FindNearestNode needs the map-marker reference,
        // which is a main-thread read.
        std::unordered_map<std::size_t, std::size_t> g_nodeSlot;
        std::vector<float> g_roadNorm; // slot-major, slots x slots
        std::size_t g_slots = 0;

        constexpr float kUnreachable = -1.0f;

        // A location's own map marker, or the nearest one up its parent
        // chain. A house interior has no marker of its own; the city it
        // sits in does, and for measuring how far someone must travel the
        // city is the right granularity anyway.
        RE::TESObjectREFR* MarkerFor(RE::FormID locationId)
        {
            auto* location = RE::TESForm::LookupByID<RE::BGSLocation>(locationId);
            // Bounded rather than while(location): parent chains are
            // authored data and a mod can make one circular.
            for (int hops = 0; location != nullptr && hops < 8; ++hops) {
                if (auto marker = location->worldLocMarker.get()) {
                    return marker.get();
                }
                location = location->parentLoc;
            }
            return nullptr;
        }

        // Split out so a failure says WHICH stage failed. The two are
        // very different problems -- a location with no map marker
        // anywhere up its parent chain is authored data we cannot
        // help, while every location failing at once is a timing bug
        // in when we asked.
        enum class PlacementFailure : std::uint8_t
        {
            None,
            NoMarker,
            NoWorldSpace,
            NoNode
        };

        std::size_t NodeForSettlement(RE::FormID settlement, PlacementFailure& why)
        {
            why = PlacementFailure::None;
            auto* marker = MarkerFor(settlement);
            if (marker == nullptr) {
                why = PlacementFailure::NoMarker;
                return PlotCasting::kNoRoadNode;
            }
            // GetWorldspace(), not GetParentCell()->worldSpace.
            //
            // `parentCell` is a plain field that the engine fills in when
            // a reference is attached to a loaded cell, so for a map
            // marker in a worldspace nobody has visited it is simply
            // null -- which is 53 of Skyrim's 61 settlements at the
            // moment we ask. GetWorldspace() is the engine's own
            // accessor and answers for an unloaded reference too.
            auto* worldSpace = marker->GetWorldspace();
            if (worldSpace == nullptr) {
                why = PlacementFailure::NoWorldSpace;
                return PlotCasting::kNoRoadNode;
            }
            const auto position = marker->GetPosition();
            const auto node = TravelGraph::FindNearestNode(worldSpace->GetFormID(), position.x, position.y);
            if (node == TravelGraph::kInvalidNode) {
                why = PlacementFailure::NoNode;
                return PlotCasting::kNoRoadNode;
            }
            return node;
        }

        // Resolve every member onto the graph, then measure every pair.
        void BuildRoadTable()
        {
            g_nodeSlot.clear();
            g_roadNorm.clear();
            g_slots = 0;

            if (TravelGraph::NodeCount() == 0) {
                logger::info("PlotPopulation: road graph unavailable; travel falls back to the hold proxy.");
                return;
            }

            // Settlements, not members: sixty-odd lookups instead of
            // nine hundred, and every resident of a town is the same
            // distance from everywhere else as their neighbours.
            std::unordered_map<RE::FormID, std::size_t> settlementNode;
            std::unordered_map<std::size_t, std::size_t> residents;
            std::size_t placed = 0;
            std::size_t noMarker = 0;
            std::size_t noWorldSpace = 0;
            std::size_t noNode = 0;
            for (auto& member : g_population.members) {
                const auto* participant = GossipGraph::Find(member.npc);
                if (participant == nullptr || participant->settlement == 0) {
                    continue;
                }
                auto it = settlementNode.find(participant->settlement);
                if (it == settlementNode.end()) {
                    auto why = PlacementFailure::None;
                    it =
                        settlementNode.emplace(participant->settlement, NodeForSettlement(participant->settlement, why))
                            .first;
                    switch (why) {
                    case PlacementFailure::NoMarker:
                        ++noMarker;
                        break;
                    case PlacementFailure::NoWorldSpace:
                        ++noWorldSpace;
                        break;
                    case PlacementFailure::NoNode:
                        ++noNode;
                        break;
                    case PlacementFailure::None:
                        break;
                    }
                }
                member.roadNode = it->second;
                if (member.roadNode != PlotCasting::kNoRoadNode) {
                    ++placed;
                    g_nodeSlot.try_emplace(member.roadNode, g_nodeSlot.size());
                    ++residents[member.roadNode];
                }
            }

            g_slots = g_nodeSlot.size();
            if (g_slots == 0) {
                logger::warn("PlotPopulation: no settlement resolved onto the road graph, so travel falls back to "
                             "the hold proxy. Of {} settlement(s): {} had no map marker, {} had a marker in no "
                             "worldspace, {} found no node near it.",
                             settlementNode.size(),
                             noMarker,
                             noWorldSpace,
                             noNode);
                return;
            }
            if (noMarker + noWorldSpace + noNode > 0) {
                logger::info("PlotPopulation: {} settlement(s) stayed off the road graph ({} no marker, {} no "
                             "worldspace, {} no node); their residents use the hold proxy.",
                             noMarker + noWorldSpace + noNode,
                             noMarker,
                             noWorldSpace,
                             noNode);
            }

            // One Dijkstra per occupied node, not per pair.
            std::vector<std::size_t> slotNode(g_slots, 0);
            for (const auto& [node, slot] : g_nodeSlot) {
                slotNode[slot] = node;
            }

            // Each pair is weighted by how likely a dispatch is to draw
            // it -- the product of the two settlements' resident counts --
            // so the calibration below describes the journeys the
            // simulation will actually make, not the ones the map merely
            // permits.
            std::vector<float> raw(g_slots * g_slots, kUnreachable);
            std::vector<std::pair<float, double>> weighted;
            weighted.reserve(g_slots * g_slots);
            for (std::size_t slot = 0; slot < g_slots; ++slot) {
                const auto field = TravelGraph::DistanceField(slotNode[slot]);
                for (std::size_t other = 0; other < g_slots; ++other) {
                    const std::size_t node = slotNode[other];
                    if (node >= field.size() || !std::isfinite(field[node])) {
                        continue;
                    }
                    raw[slot * g_slots + other] = field[node];
                    if (slot != other) {
                        const double weight = static_cast<double>(residents[slotNode[slot]])
                                              * static_cast<double>(residents[slotNode[other]]);
                        weighted.emplace_back(field[node], weight);
                    }
                }
            }

            if (weighted.empty()) {
                logger::info("PlotPopulation: road graph has no connected settlement pairs; using the hold proxy.");
                g_slots = 0;
                g_nodeSlot.clear();
                return;
            }

            // Calibrated so the TYPICAL journey scores 0.5, by taking the
            // population-weighted median pair and calling it half the
            // scale. Anything past twice that is simply "far".
            //
            // A percentile of the raw pair list would have been simpler
            // and wrong in a way that matters: it describes the map's
            // geometry rather than the traffic over it, so a province
            // whose cities happen to sit far apart would push almost
            // every real dispatch to the top of the range and flatten the
            // variation this whole term exists to provide -- which is the
            // failure the hold proxy had, arrived at from the other side.
            std::sort(weighted.begin(), weighted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            double total = 0.0;
            for (const auto& [distance, weight] : weighted) {
                total += weight;
            }
            float median = weighted.back().first;
            double seen = 0.0;
            for (const auto& [distance, weight] : weighted) {
                seen += weight;
                if (seen >= 0.5 * total) {
                    median = distance;
                    break;
                }
            }
            const float reference = std::max(1.0f, 2.0f * median);

            g_roadNorm.assign(g_slots * g_slots, kUnreachable);
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] >= 0.0f) {
                    g_roadNorm[i] = std::min(1.0f, raw[i] / reference);
                }
            }

            logger::info("PlotPopulation: road distances over {} settlement node(s), {} member(s) placed; "
                         "typical journey {:.0f} units -> norm 0.50, full scale {:.0f}, longest pair {:.0f}",
                         g_slots,
                         placed,
                         median,
                         reference,
                         weighted.back().first);
        }

        // The factions plots consider "prominent enough to command
        // something", by the same size filter gossip already applies to
        // its own social edges. Reusing that set rather than inventing a
        // second definition means one answer to "which organisations
        // matter", not two that can drift.
        //
        // GossipGraph does not expose the admitted set directly, but it
        // does name admitted factions and marks shared-faction edges, so
        // the set is recoverable from the edges themselves.
        std::unordered_set<RE::FormID> AdmittedFactions()
        {
            std::unordered_set<RE::FormID> admitted;
            for (const RE::FormID npc : GossipGraph::Participants()) {
                for (const auto& edge : GossipGraph::PersonalEdges(npc)) {
                    if (edge.sharedFaction && edge.faction != 0) {
                        admitted.insert(edge.faction);
                    }
                }
            }
            return admitted;
        }

        bool IsMemberOf(RE::TESNPC* npc, RE::FormID faction)
        {
            if (npc == nullptr || faction == 0) {
                return false;
            }
            for (const auto& rank : npc->factions) {
                if (rank.faction != nullptr && rank.faction->GetFormID() == faction) {
                    return true;
                }
            }
            return false;
        }

        // Rank of `npc` in `faction`, or nullopt when not a member.
        //
        // Read off the TESNPC's own faction list rather than through an
        // Actor: the base form carries the authored membership, which is
        // what "who belongs to what" means here, and it does not require
        // a loaded reference.
        int RankIn(RE::TESNPC* npc, RE::TESFaction* faction, bool& isMember)
        {
            isMember = false;
            if (npc == nullptr || faction == nullptr) {
                return 0;
            }
            for (const auto& rank : npc->factions) {
                if (rank.faction == faction) {
                    isMember = true;
                    return static_cast<int>(rank.rank);
                }
            }
            return 0;
        }
        // Authored competence, normalised. Level is the broad signal;
        // the three skills are the ones PlotResolution::Suitability
        // actually consults, and reading only those keeps this a short
        // walk rather than a copy of Skyrim's whole skill tree.
        //
        // Level is normalised against 50 rather than 100: a level-50
        // NPC is already exceptional in vanilla, and normalising against
        // the theoretical maximum would squash almost the entire
        // population into the bottom quarter of the range.
        PlotCasting::SkillProfile ReadSkills(RE::TESNPC* npc)
        {
            PlotCasting::SkillProfile profile;
            if (npc == nullptr) {
                return profile;
            }

            constexpr double kLevelCeiling = 50.0;
            profile.competence = std::clamp(static_cast<double>(npc->actorData.level) / kLevelCeiling, 0.0, 1.0);

            if (npc->playerSkills.values != nullptr) {
                const auto skill = [npc](RE::ActorValue av) {
                    const auto index =
                        static_cast<std::size_t>(av) - static_cast<std::size_t>(RE::ActorValue::kOneHanded);
                    if (index >= 18) {
                        return 0.5;
                    }
                    return std::clamp(static_cast<double>(npc->playerSkills.values[index]) / 100.0, 0.0, 1.0);
                };
                profile.speech = skill(RE::ActorValue::kSpeech);
                profile.sneak = skill(RE::ActorValue::kSneak);
                profile.pickpocket = skill(RE::ActorValue::kPickpocket);
            }
            return profile;
        }
    } // namespace

    void Build()
    {
        if (g_ready) {
            return;
        }
        if (!Settings::Get().plotsEnabled) {
            return;
        }
        if (!GossipGraph::IsReady()) {
            logger::warn("PlotPopulation: gossip graph unavailable; plots will have no population");
            g_ready = true;
            return;
        }

        const auto admitted = AdmittedFactions();

        // Resolve each admitted faction once rather than per participant.
        std::unordered_map<RE::FormID, RE::TESFaction*> factionForms;
        factionForms.reserve(admitted.size());
        for (const RE::FormID id : admitted) {
            if (auto* form = RE::TESForm::LookupByID<RE::TESFaction>(id)) {
                factionForms.emplace(id, form);
            }
        }

        const auto& participants = GossipGraph::Participants();
        g_population.members.clear();
        g_population.members.reserve(participants.size());

        std::size_t withFactions = 0;
        for (const RE::FormID npcId : participants) {
            const auto* participant = GossipGraph::Find(npcId);
            if (participant == nullptr) {
                continue;
            }

            PlotCasting::Member member;
            member.npc = npcId;
            member.name = GossipGraph::NpcName(npcId);
            member.hold = participant->hold;

            if (auto* npcForm = RE::TESForm::LookupByID<RE::TESNPC>(npcId)) {
                member.skills = ReadSkills(npcForm);

                // Gossip's size-filtered set is the FALLBACK: membership
                // of one of these says "belongs to an organisation" and
                // nothing about rank, which is why standing stays 0.
                for (const auto& [factionId, factionForm] : factionForms) {
                    bool isMember = false;
                    RankIn(npcForm, factionForm, isMember);
                    if (isMember) {
                        member.factions.push_back({factionId, 0.0, false});
                    }
                }

                // The roster is the source of truth for hierarchy. A
                // rostered faction either replaces the fallback entry or
                // is added outright, because the roster may well name a
                // faction gossip's size filter never admitted — the
                // Imperial Legion has 288 members and the filter stops
                // at 40.
                for (const auto& entry : PlotFactionRoster::Entries()) {
                    // Under Ranked, being NAMED is itself a way in. The
                    // ladder is the membership there, so a Member line for
                    // someone the primary faction misses should mean what
                    // it says rather than silently doing nothing --
                    // Whiterun's replacement steward is a Companions
                    // servant who is in no Whiterun faction at all.
                    const bool named = entry.membership == PlotFactionRoster::Membership::Ranked
                                       && std::any_of(entry.overrides.begin(),
                                                      entry.overrides.end(),
                                                      [npcId](const PlotFactionRoster::Override& o) {
                                                          return o.npc == npcId && o.rank > 0;
                                                      });
                    if (!named && !IsMemberOf(npcForm, entry.faction)) {
                        continue;
                    }
                    const double standing = PlotFactionRoster::StandingOf(npcId, entry.faction);
                    const auto existing = std::find_if(
                        member.factions.begin(),
                        member.factions.end(),
                        [&entry](const PlotCasting::FactionStanding& f) { return f.faction == entry.faction; });

                    // Membership = Ranked keeps only the people the ladder
                    // places. The primary faction is then a SCOPE rather
                    // than a roll: a hold court reads province-wide job
                    // factions through the hold's own crime faction, and
                    // the hold's other 90 residents are not courtiers.
                    //
                    // The erase matters as much as the skip. Gossip's
                    // size filter admits any faction under 40 members, so
                    // four of the nine hold crime factions arrive here
                    // ALREADY in the list as a peer entry; leaving that
                    // standing would keep every Falkreath farmer in the
                    // court while every Whiterun one was excluded.
                    if (entry.membership == PlotFactionRoster::Membership::Ranked && standing <= 0.0) {
                        if (existing != member.factions.end()) {
                            member.factions.erase(existing);
                        }
                        continue;
                    }

                    if (existing != member.factions.end()) {
                        existing->standing = standing;
                        existing->rostered = true;
                    } else {
                        member.factions.push_back({entry.faction, standing, true});
                    }
                }
            }
            if (!member.factions.empty()) {
                ++withFactions;
            }

            for (const auto& edge : GossipGraph::PersonalEdges(npcId)) {
                member.ties.push_back({edge.other, edge.sharedFaction});
            }

            g_population.members.push_back(std::move(member));
        }

        g_ready = true;
        logger::info("PlotPopulation: {} member(s), {} in an admitted faction, {} admitted faction(s)",
                     g_population.members.size(),
                     withFactions,
                     factionForms.size());
    }

    PlotCasting::SeatedPredicate SeatedPredicate()
    {
        // Nothing gated, nothing to ask. An EMPTY predicate rather than
        // one that always says yes: casting reads that as "everything
        // counts" and skips the call, which is every roster listing no
        // court at all.
        const auto& entries = PlotFactionRoster::Entries();
        const bool anyGate = std::any_of(entries.begin(), entries.end(), [](const PlotFactionRoster::Entry& e) {
            return e.officeFaction != 0 && !e.officeCandidates.empty();
        });
        if (!anyGate) {
            return {};
        }

        // Asked live, per membership, at the moment casting reads it.
        // Safe off the plot worker for the reasons IsSeated documents,
        // and more current than any snapshot could be: there is no
        // window between taking the answer and using it.
        return [](RE::FormID npc, RE::FormID faction) {
            const auto* entry = PlotFactionRoster::Find(faction);
            return entry == nullptr || PlotFactionRoster::IsSeated(npc, *entry);
        };
    }

    void OnSessionStart()
    {
        if (!g_ready) {
            return;
        }
        // Already placed: the graph and the markers are both static, so
        // a second session has nothing new to learn. A session that
        // placed NOBODY falls through and tries again, which is what
        // makes a bad first attempt cost one load rather than the whole
        // playthrough.
        if (g_slots > 0) {
            return;
        }
        BuildRoadTable();
    }

    bool IsReady()
    {
        return g_ready;
    }

    double RoadDistanceNorm(std::size_t nodeA, std::size_t nodeB)
    {
        if (g_slots == 0 || nodeA == PlotCasting::kNoRoadNode || nodeB == PlotCasting::kNoRoadNode) {
            return -1.0;
        }
        const auto a = g_nodeSlot.find(nodeA);
        const auto b = g_nodeSlot.find(nodeB);
        if (a == g_nodeSlot.end() || b == g_nodeSlot.end()) {
            return -1.0;
        }
        const float norm = g_roadNorm[a->second * g_slots + b->second];
        return norm < 0.0f ? -1.0 : static_cast<double>(norm);
    }

    const PlotCasting::Population& Get()
    {
        return g_population;
    }

    bool IsAlive(RE::FormID npc)
    {
        const auto* participant = GossipGraph::Find(npc);
        if (participant == nullptr || participant->actorRef == 0) {
            return false;
        }
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(participant->actorRef);
        if (actor == nullptr) {
            return false;
        }
        // AsActorState() rather than the inherited GetLifeState(): the
        // ActorState base sits at a different offset on AE than on SE,
        // and AsActorState does that relocation. Calling the inherited
        // method directly reads the wrong bytes on one runtime. Same
        // reasoning as GossipSim's own availability check.
        const auto* state = actor->AsActorState();
        if (state == nullptr) {
            return false;
        }
        if (state->GetLifeState() != RE::ACTOR_LIFE_STATE::kAlive) {
            return false;
        }
        // Disabled is transient — a quest can re-enable them — but an
        // NPC who is not in the world cannot be carrying out a step, so
        // they are not castable while it lasts.
        return !actor->IsDisabled();
    }

    bool IsNearPlayer(RE::FormID npc)
    {
        const auto* participant = GossipGraph::Find(npc);
        if (participant == nullptr || participant->actorRef == 0) {
            return false;
        }
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(participant->actorRef);
        if (actor == nullptr) {
            return false;
        }
        // "Near the player" means "the engine has their 3D loaded",
        // which is exactly the set of actors the player could plausibly
        // be watching. A distance check would be both more expensive and
        // less correct — an actor thirty feet away through a wall in an
        // unloaded cell is not being watched.
        //
        // Unique NPCs' Actor objects are persistent and always resident;
        // only their 3D unloads. That is what makes this a plain bool
        // load, safe off the main thread.
        return actor->Is3DLoaded();
    }

    PlotCasting::AlivePredicate AlivePredicate()
    {
        return [](RE::FormID npc) { return IsAlive(npc); };
    }
} // namespace NarrativeEngine::PlotPopulation
