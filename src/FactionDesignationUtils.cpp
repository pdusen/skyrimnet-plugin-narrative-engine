#include <FactionDesignationUtils.h>

#include <logger.h>

#include <RE/Skyrim.h>

#include <cstddef>

namespace NarrativeEngine::FactionDesignationUtils
{
    void SweepStaleDesignated(RE::TESFaction*  fact,
                              RE::Actor*       target,
                              std::int8_t      designatedRank,
                              std::int8_t      candidateRank,
                              std::string_view logTag)
    {
        if (!fact) return;
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) return;

        std::size_t swept = 0;
        const auto walk = [&](const RE::BSTArray<RE::ActorHandle>& list) {
            for (const auto& h : list) {
                auto ref = h.get();
                auto* actor = ref.get();
                if (!actor || actor == target) continue;
                // GetFactionRank's `a_isPlayer` argument is only consulted
                // by the engine for the player-crime path — false is
                // correct for all other actors.
                if (actor->GetFactionRank(fact, false) >= designatedRank) {
                    actor->AddToFaction(fact, candidateRank);
                    ++swept;
                    logger::info(
                        "{}[FACTION]: swept stale rank-{}+ member 0x{:08X} "
                        "-> rank {}",
                        logTag, static_cast<int>(designatedRank),
                        actor->GetFormID(),
                        static_cast<int>(candidateRank));
                }
            }
        };
        walk(pl->highActorHandles);
        walk(pl->middleHighActorHandles);
        walk(pl->middleLowActorHandles);
        walk(pl->lowActorHandles);
        if (swept > 0) {
            logger::info(
                "{}[FACTION]: pre-dispatch sweep demoted {} stale rank-{}+ actor(s)",
                logTag, swept, static_cast<int>(designatedRank));
        }
    }

    void PromoteToDesignated(RE::TESFaction*  fact,
                             RE::Actor*       sender,
                             std::int8_t      designatedRank,
                             std::int8_t      candidateRank,
                             std::string_view logTag)
    {
        if (!fact || !sender) return;
        SweepStaleDesignated(fact, sender, designatedRank, candidateRank, logTag);
        const auto currentRank = sender->GetFactionRank(fact, false);
        if (currentRank == designatedRank) {
            logger::info(
                "{}[FACTION]: sender 0x{:08X} already at rank {}; no "
                "promotion needed", logTag, sender->GetFormID(),
                static_cast<int>(designatedRank));
            return;
        }
        // AddToFaction serves as both add-and-set and set-rank in
        // CommonLibSSE-NG (same underlying engine function). Updates the
        // ExtraFactionChanges record in place.
        sender->AddToFaction(fact, designatedRank);
        if (currentRank < 0) {
            logger::info(
                "{}[FACTION]: added sender 0x{:08X} to faction at rank {}",
                logTag, sender->GetFormID(),
                static_cast<int>(designatedRank));
        } else {
            logger::info(
                "{}[FACTION]: promoted sender 0x{:08X} from rank {} to rank {}",
                logTag, sender->GetFormID(), currentRank,
                static_cast<int>(designatedRank));
        }
    }

    void DemoteToCandidate(RE::TESFaction*  fact,
                           RE::Actor*       sender,
                           std::int8_t      candidateRank,
                           std::string_view logTag)
    {
        if (!fact || !sender) return;
        const auto currentRank = sender->GetFactionRank(fact, false);
        if (currentRank < 0) return;
        if (currentRank == candidateRank) return;
        sender->AddToFaction(fact, candidateRank);
        logger::info(
            "{}[FACTION]: demoted sender 0x{:08X} from rank {} back to rank {}",
            logTag, sender->GetFormID(), currentRank,
            static_cast<int>(candidateRank));
    }
}
