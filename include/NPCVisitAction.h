#pragma once

#include <IAction.h>

#include <SKSE/SKSE.h>

#include <RE/Skyrim.h>

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

    namespace NPCVisitAction_Cooldowns
    {
        // Stamp the per-sender cooldown for `senderNpcFormID`. Called
        // when a visit's Salutation → Discuss transition fires — the
        // moment we know the sender actually arrived at the player
        // and delivered their opening line. Rolled-back visits
        // (Salutation timeout, alias-fill failure, hard-abort before
        // Discuss) intentionally do NOT stamp. Silently no-ops if
        // `senderNpcFormID == 0`.
        void OnVisitCompleted(RE::FormID senderNpcFormID);

        // Filter helper — returns true if this sender is currently
        // within their per-sender cooldown window. Called by
        // VisitComposer during candidate viability filtering. Also
        // returns false if `iVisitSenderCooldownGameHours <= 0`
        // (cooldown disabled).
        bool IsSenderOnCooldown(RE::FormID senderNpcFormID);
    }

    namespace NPCVisitAction_Persistence
    {
        // SKSE co-save record type ID for the visit action's per-
        // sender cooldown table. Frozen — changing it would orphan
        // previously-saved data.
        inline constexpr std::uint32_t kRecordTypeId = 'NEVC';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc,
                    std::uint32_t                 version,
                    std::uint32_t                 length);
        void OnRevert();
    }
}
