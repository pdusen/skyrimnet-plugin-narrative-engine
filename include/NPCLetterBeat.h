#pragma once

#include <IBeat.h>

#include <SKSE/SKSE.h>

#include <cstddef>
#include <cstdint>

// NPCLetterBeat — the Narrative Beat System's Either-polarity "someone
// writes to you" beat. Composes a letter via LetterComposer, then hands
// it off to vanilla WICourier for delivery. Tone / polarity are driven
// by the content the LLM generates; this beat serves both raise- and
// lower-direction letters.
//
// Lifecycle (four-state per-beat model, see PHASE_06 doc):
//   COMPOSE — validate params, fire the compose LLM, allocate a pool
//             slot, promote sender via the marker faction, EnsureQuestStarted,
//             and poll the Sender / LetterRef alias fills. On success →
//             RUNNING; on any failure → CLEANUP with failure_reason.
//   RUNNING — wait a verify window; then confirm the letter reached
//             WICourierContainerRef. On confirmation → CLEANUP with
//             success. On timeout → CLEANUP with dispatch_verify_failed.
//   CLEANUP — stamp per-beat cooldown on success paths, advance the
//             per-slot quest to its post-dispatch stage or roll it
//             back, then return to NOT_RUNNING. The rest of the letter
//             lifecycle (delivered → read → disposed) continues via
//             LetterPool's own sinks after the beat has ended.
namespace NarrativeEngine
{
    class NPCLetterBeat : public IBeat
    {
    public:
        std::string Name() const override;
        std::string Description() const override;
        BeatPolarity Polarity() const override;
        bool IsAvailable(const BeatContext& ctx) const override;
        void OnStart(const BeatContext& ctx, const nlohmann::json& parameters) override;
        TickResult Tick(TickMode mode, BeatState state) override;
        double RemainingCooldownGameHours() const override;
    };

    namespace NPCLetterBeat_Init
    {
        // Resolve the 20 `_ne_PooledLetterQuestNN` EditorIDs into the
        // per-slot delivery-quest cache, and warm the vanilla WICourier /
        // courier-container resolution so the first dispatch pass
        // doesn't lazy-lookup on the main thread.
        //
        // Must run AFTER LetterPool::Initialize at kDataLoaded, because
        // the per-slot quest array is keyed against LetterPool slot
        // indices.
        //
        // Idempotent — second call rewires nothing and is cheap.
        void Initialize();
    } // namespace NPCLetterBeat_Init

    namespace NPCLetterBeat_QuestControl
    {
        // Advance the per-slot delivery quest to a specific stage. Called
        // by LetterPool from player-driven lifecycle transitions
        // (delivered → 30, read → 40, disposed → 50). Silently no-ops if
        // `slotIndex` is out of range or its quest didn't resolve at
        // kDataLoaded.
        void AdvanceSlotStage(std::size_t slotIndex, std::uint32_t stage);

        // Synchronously Stop() + Reset() the per-slot delivery quest so
        // it's ready for the next allocation. Used by the allocator's
        // recycle path: the next dispatch on this slot lands in the same
        // frame, and the quest's own async Shutdown fragment chain
        // wouldn't finish before the fresh EnsureQuestStarted races it,
        // so the native Stop+Reset unwinds synchronously instead.
        void ShutdownSlotQuestSync(std::size_t slotIndex);

        // Delete the LetterRef alias's spawned REFR for the given slot.
        // The alias tracks the letter REFR wherever it lives (sender
        // inventory → courier → player → merchant chest / world drop),
        // so a single Disable+SetDelete removes the letter from the
        // world entirely.
        //
        // MUST be called BEFORE ShutdownSlotQuestSync: quest.Reset()
        // clears alias fills, after which the LetterRef alias no
        // longer points at anything and this helper silently no-ops.
        void DeleteLetterRef(std::size_t slotIndex);

        // Ask vanilla WICourier to release its tracking of the given
        // slot's letter REFR. Fires
        // WICourierScript.removeRefFromContainer(letterRef, false) via
        // VM dispatch. Needed alongside DeleteLetterRef to keep the
        // WICourierItemCount global in sync.
        //
        // MUST be called BEFORE ShutdownSlotQuestSync — the VM call
        // needs the LetterRef alias filled to produce the REFR argument.
        void ReleaseLetterFromCourier(std::size_t slotIndex);
    } // namespace NPCLetterBeat_QuestControl

    namespace NPCLetterBeat_Cooldowns
    {
        // Called by LetterPool from its MarkDelivered path once the
        // vanilla courier has handed a letter to the player. Stamps the
        // per-sender cooldown so this NPC won't be picked as a candidate
        // again for `iLetterSenderCooldownGameHours` in-game hours.
        void OnLetterDelivered(RE::FormID senderNpcFormID);

        // Filter helper — returns true if this sender is currently
        // within their per-sender cooldown window. Called by
        // LetterComposer during candidate filtering.
        bool IsSenderOnCooldown(RE::FormID senderNpcFormID);
    } // namespace NPCLetterBeat_Cooldowns

    namespace NPCLetterBeat_Persistence
    {
        // Per-beat cosave record. Frozen — changing it orphans data.
        inline constexpr std::uint32_t kRecordTypeId = 'NBLP';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
        void OnRevert();
    } // namespace NPCLetterBeat_Persistence
} // namespace NarrativeEngine
