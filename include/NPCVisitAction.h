#pragma once

#include <IAction.h>

// NPCVisitAction — the Director's face-to-face social lever.
//
// A known NPC (chosen by the compose LLM from the player's recent
// engagement history) is warped to a nearby out-of-sight XMarker,
// walks up to the player under a Follow package, and holds an
// in-person conversation whose turns are driven through SkyrimNet's
// ExecuteAction API. See PHASE_05_NPC_VISIT_ACTION.md for the full
// design.
//
// Phase 05 Step 6 ships only the skeleton — metadata + IsAvailable +
// a Start-stub that returns started=false. Step 8 wires up the real
// dispatch chain (compose → snapshot → anchor placement → faction
// promote → EnsureQuestStarted) plus the Salutation-timeout rollback.
namespace NarrativeEngine
{
    class NPCVisitAction : public IAction
    {
    public:
        std::string    Name()        const override;
        std::string    Description() const override;
        ActionPolarity Polarity()    const override;
        bool           IsAvailable(const ActionContext& ctx) const override;
        StartResult    Start(const ActionContext& ctx,
                             const nlohmann::json& parameters) override;
    };
}
