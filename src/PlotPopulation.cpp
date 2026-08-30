#include <PlotPopulation.h>

#include <GossipGraph.h>
#include <logger.h>
#include <Settings.h>

#include <unordered_map>
#include <unordered_set>

namespace NarrativeEngine::PlotPopulation
{
    namespace
    {
        PlotCasting::Population g_population;
        bool g_ready = false;

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
                for (const auto& [factionId, factionForm] : factionForms) {
                    bool isMember = false;
                    const int rank = RankIn(npcForm, factionForm, isMember);
                    if (isMember) {
                        member.factions.push_back({factionId, rank});
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

    bool IsReady()
    {
        return g_ready;
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

    PlotCasting::AlivePredicate AlivePredicate()
    {
        return [](RE::FormID npc) { return IsAlive(npc); };
    }
} // namespace NarrativeEngine::PlotPopulation
