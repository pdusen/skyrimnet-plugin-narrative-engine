#pragma once

#include <IAction.h>

#include <SKSE/SKSE.h>

// NPCLetterAction — the Director's first social lever and its first
// Either-polarity action. Composes a letter via LetterComposer, then
// VM-dispatches to vanilla `WICourierScript.AddItemToContainer` so the
// existing courier system delivers it. Tone and polarity are driven by
// the content the LLM generates; the action itself is one entry point
// for both raise- and lower-direction letters.
//
// Phase 04 Step 11 ships only the skeleton: name / description /
// polarity / IsAvailable. `Start` is a stub that fails immediately;
// Step 13 wires up the real Compose → dispatch → verify chain plus
// the IAction::DetectAndRollbackFailedStart / DetectCompletion polls.
namespace NarrativeEngine
{
    class NPCLetterAction : public IAction
    {
    public:
        std::string    Name()        const override;
        std::string    Description() const override;
        ActionPolarity Polarity()    const override;
        bool           IsAvailable(const ActionContext& ctx) const override;
        StartResult    Start(const ActionContext& ctx, const nlohmann::json& parameters) override;
        bool           DetectAndRollbackFailedStart(const ActionContext& ctx,
                                                    double                secondsSinceStart) override;
        bool           DetectCompletion(const ActionContext& ctx,
                                        double                secondsSinceStart) override;
    };

    namespace NPCLetterAction_Init
    {
        // Resolve the 20 `_ne_PooledLetterQuestNN` EditorIDs into the
        // per-slot delivery-quest cache, and warm the vanilla WICourier /
        // courier-container resolution so the verification polls don't
        // have to do the lookup lazily on first use.
        //
        // Must run AFTER LetterPool::Initialize at kDataLoaded, because
        // the per-slot quest array is keyed against LetterPool slot
        // indices.
        //
        // Idempotent — second call rewires nothing and is cheap.
        void Initialize();
    }

    namespace NPCLetterAction_QuestControl
    {
        // Advance the per-slot delivery quest to a specific stage. Called
        // by LetterPool from player-driven lifecycle transitions
        // (delivered → 30, read → 40, disposed → 50). Silently no-ops if
        // `slotIndex` is out of range or its quest didn't resolve at
        // kDataLoaded. Safe to call on a stopped quest — the underlying
        // VM-dispatch is fire-and-forget and Papyrus's SetStage is a
        // no-op on a non-running quest.
        void AdvanceSlotStage(std::size_t slotIndex, std::uint32_t stage);

        // Synchronously Stop() + Reset() the per-slot delivery quest so
        // it's ready for the next allocation of this slot. Used by the
        // allocator's recycle path — which is followed immediately (same
        // frame) by PopulateSlot + EnsureQuestStarted, so the async
        // Stage 50/60 → 200 → Shutdown fragment chain would race the
        // new dispatch. This shortcut uses the native `Stop()` +
        // `Reset()` methods on TESQuest, which do exactly what Papyrus's
        // Shutdown() does but synchronously. Safe on an already-stopped
        // quest (Stop and Reset are idempotent).
        void ShutdownSlotQuestSync(std::size_t slotIndex);
    }

    namespace NPCLetterAction_Cooldowns
    {
        // Called by LetterPool from its MarkDelivered path once the
        // vanilla courier has handed a letter to the player. Stamps the
        // per-sender cooldown so this NPC won't be picked as a candidate
        // again for `iLetterSenderCooldownGameHours` in-game hours.
        // Silently no-ops if `senderNpcFormID == 0`.
        void OnLetterDelivered(RE::FormID senderNpcFormID);

        // Filter helper — returns true if this sender is currently
        // within their per-sender cooldown window. Called by
        // LetterComposer during candidate filtering. Also returns false
        // if `iLetterSenderCooldownGameHours <= 0` (cooldown disabled).
        bool IsSenderOnCooldown(RE::FormID senderNpcFormID);
    }

    namespace NPCLetterAction_Persistence
    {
        // SKSE co-save record type ID for the letter action's cooldown
        // state (global action cooldown stamp + per-sender delivery
        // stamps). Frozen — changing it would orphan previously-saved
        // data.
        inline constexpr std::uint32_t kRecordTypeId = 'NELE';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
        void OnRevert();
    }
}
