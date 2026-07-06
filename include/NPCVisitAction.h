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
// Phase 05 Step 8 wires the real dispatch chain: compose LLM call →
// snapshot → faction promote → EnsureQuestStarted, plus
// DetectAndRollbackFailedStart for the Salutation timeout. Later
// steps wire Salutation→Discuss (Step 9), the poll gate (Step 10),
// three-branch handling (Step 11), Valediction→ReturnHome (Step 12),
// ReturnHome watchdog (Step 13), OnHold/ReEngage (Step 14), and the
// hard-abort (Step 15).
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
        bool           DetectAndRollbackFailedStart(const ActionContext& ctx,
                                                     double                secondsSinceStart) override;
        bool           DetectCompletion(const ActionContext& ctx,
                                         double                secondsSinceStart) override;
    };

    namespace NPCVisitAction_Init
    {
        // Resolve `_ne_VisitQuest`, `_ne_VisitSenderFaction`, and the
        // three reference aliases (Sender, SpawnMarker, ReturnAnchor).
        // Called at kDataLoaded after Settings::Load, so downstream
        // subsystems can consult the resolved handles.
        //
        // If any critical form fails to resolve, IsAvailable is
        // permanently disabled — the log line names the missing form
        // so it's diagnosable from the SKSE log alone.
        void Initialize();
    }
}
