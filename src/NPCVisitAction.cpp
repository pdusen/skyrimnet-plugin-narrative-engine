#include <NPCVisitAction.h>

#include <LocationKeywords.h>
#include <SenderCandidatePool.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <VisitComposer.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/T/TESNPC.h>

namespace NarrativeEngine
{
    namespace
    {
        // Visit-specific candidate viability filter used by
        // IsAvailable's cheap CountViable check. Kept in sync with
        // VisitComposer's filter so the count matches what Compose()
        // will end up building.
        bool VisitViabilityFilter_ForCountViable(RE::Actor* actor,
                                                 std::string* skipReasonOut)
        {
            if (!actor) {
                if (skipReasonOut) *skipReasonOut = "missing-actor";
                return false;
            }
            if (auto* base = actor->GetActorBase()) {
                if (!base->IsUnique()) {
                    if (skipReasonOut) *skipReasonOut = "not-unique";
                    return false;
                }
            }
            if (actor->IsInCombat()) {
                if (skipReasonOut) *skipReasonOut = "in-combat";
                return false;
            }
            if (actor->IsPlayerTeammate()) {
                if (skipReasonOut) *skipReasonOut = "player-follower";
                return false;
            }
            if (!actor->GetCurrentLocation()) {
                if (skipReasonOut) *skipReasonOut = "no-current-location";
                return false;
            }
            return true;
        }
    }

    std::string NPCVisitAction::Name() const
    {
        return "npc_visit";
    }

    std::string NPCVisitAction::Description() const
    {
        return
            "An NPC the player knows drops what they were doing and travels "
            "to the player's current location to speak in person. Best fit "
            "when the intended beat cannot survive being written down and "
            "folded into a letter — an urgent warning, an apology that needs "
            "eye contact, a confession, a threat delivered face-to-face — or "
            "when the sender's own state (grief, anger, love, contrition) "
            "demands they show up rather than write. Tone and polarity are "
            "driven by the generated content, so this action can serve "
            "either a raising direction (menacing / urgent visits) or a "
            "lowering direction (contrite / warm / mournful visits) "
            "depending on what the current phase calls for.\n"
            "\n"
            "Avoid when the player is in a dungeon, lair, jail cell, arena, "
            "or other cell where a stranger walking up would be jarring — "
            "the action already gates itself on those. Also avoid when the "
            "player has just received a letter or another visit; letting the "
            "cadence breathe between social beats reads more naturally.\n"
            "\n"
            "Prefer `npc_letter` when the beat could plausibly land on paper "
            "and reach the player at the courier's schedule. Prefer "
            "`npc_visit` when the sender needs to see the player's reaction, "
            "when the information is dangerous to write down, or when the "
            "situation is urgent and needs an answer now.\n"
            "\n"
            "Parameters:\n"
            "  - `urgency_hint` (optional, string): `low` / `medium` / "
            "`high`. Defaults to `medium`. One input among several to the "
            "brief-composition prompt; not a hard directive.\n"
            "\n"
            "Do NOT include other parameter fields — sender, briefing, "
            "topic, mood, and tags are decided by the action's own "
            "compose LLM call. Extra fields will be silently ignored.";
    }

    ActionPolarity NPCVisitAction::Polarity() const
    {
        return ActionPolarity::Either;
    }

    bool NPCVisitAction::IsAvailable(const ActionContext& ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("NPCVisitAction::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        // Location gate — dungeons/lairs/jails/arenas/barracks.
        if (ctx.player) {
            if (LocationKeywords::IsVisitHostile(ctx.player->GetCurrentLocation())) {
                return blocked("LocationKeywords::IsVisitHostile");
            }
        }

        if (!SkyrimNetAPI::IsMemorySystemReady()) {
            return blocked("SkyrimNet memory system not ready");
        }

        // Candidate-count check — cheap CountViable walk (no per-candidate
        // memory fetch). Uses the same viability filter as the compose
        // path so the count matches what Compose() will actually see.
        const std::size_t minCandidates =
            static_cast<std::size_t>(
                std::max(1, Settings::Get().visitMinSenderCandidates));
        const std::size_t viable = SenderCandidatePool::CountViable(
            &VisitViabilityFilter_ForCountViable, minCandidates);
        if (viable < minCandidates) {
            if (debug) {
                logger::debug(
                    "NPCVisitAction::IsAvailable: blocked (only {} viable "
                    "candidates, need {})",
                    viable, minCandidates);
            }
            return false;
        }

        return true;
    }

    StartResult NPCVisitAction::Start(const ActionContext& /*ctx*/,
                                       const nlohmann::json& /*parameters*/)
    {
        // Step 6 stub — real dispatch chain wires in at Step 8 (compose
        // → snapshot → anchor placement → faction promote →
        // EnsureQuestStarted). For now the dispatcher will see this as a
        // failed start and unwind; no CK content is needed for the
        // action to be selectable and log its stub line.
        logger::info(
            "NPCVisitAction::Start: stub (Step 6) — real dispatch chain arrives at Step 8");
        return {false, "npc_visit Start not yet implemented"};
    }
}
