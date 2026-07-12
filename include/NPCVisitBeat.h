#pragma once

#include <IBeat.h>

#include <SKSE/SKSE.h>

#include <RE/Skyrim.h>

// NPCVisitBeat — the Narrative Beat System's face-to-face social beat.
//
// A known NPC (chosen by the beat-select LLM from the player's recent
// engagement history) is warped to a nearby out-of-sight XMarker, walks
// up to the player under a Follow package, and holds an in-person
// conversation whose turns are driven through SkyrimNet's ExecuteAction
// API. See PHASE_05_NPC_VISIT_ACTION.md for the underlying design and
// PHASE_06_BEAT_SYSTEM_REFACTOR.md for the beat-lifecycle refactor.
//
// Lifecycle (four-state per-beat model):
//   COMPOSE — fire compose LLM, promote sender via marker faction,
//             snapshot pre-dispatch pose, EnsureQuestStarted, verify
//             alias fills. On success → RUNNING (quest already at
//             Stage 10 = Salutation); on any failure → CLEANUP with
//             failure_reason.
//   RUNNING — dispatches on quest stage each Normal-mode Tick:
//               Stage 10 (Salutation) — approach-distance / timeout
//               Stage 20 (Discuss)    — speech sampler + gate tick +
//                                       poll fire + verdict handling
//               Stage 25 (OnHold)     — combat-stuck timeout (Combat mode)
//               Stage 27 (ReEngage)   — approach-distance / OnHold re-trip
//               Stage 30 (Valediction)— closing narration + dwell
//               Stage 50 (ReturnHome) — distance / LOS / cell / timeout
//               Stage 60 / 200        — terminal → CLEANUP
//   CLEANUP — teleport sender home if alive, demote, dispatch Shutdown
//             fragment, wait for quest to drop to Stage 0, then return
//             to NOT_RUNNING.
namespace NarrativeEngine
{
    class NPCVisitBeat : public IBeat
    {
    public:
        std::string Name() const override;
        std::string Description() const override;
        BeatPolarity Polarity() const override;
        bool IsAvailable(const BeatContext& ctx) const override;
        void OnStart(const BeatContext& ctx, const nlohmann::json& parameters) override;
        TickResult Tick(TickMode mode, BeatState state) override;
    };

    namespace NPCVisitBeat_Init
    {
        // Resolve `_ne_VisitQuest`, `_ne_VisitSenderFaction`, the three
        // reference aliases (Sender, SpawnMarker, ReturnAnchor), and
        // wire the DialogueMenu / combat / death sinks. Called at
        // kDataLoaded after Settings::Load.
        void Initialize();
    } // namespace NPCVisitBeat_Init

    namespace NPCVisitBeat_Cooldowns
    {
        // Stamp the per-sender cooldown for `senderNpcFormID`. Called
        // when the Salutation → Discuss transition fires — the moment
        // we know the sender actually arrived and delivered their
        // opening line. Rolled-back / hard-aborted visits deliberately
        // do NOT stamp.
        void OnVisitCompleted(RE::FormID senderNpcFormID);

        // Filter helper — returns true if this sender is currently
        // within their per-sender cooldown window. Called by
        // VisitComposer during candidate viability filtering.
        bool IsSenderOnCooldown(RE::FormID senderNpcFormID);
    } // namespace NPCVisitBeat_Cooldowns

    namespace NPCVisitBeat_Persistence
    {
        // Per-beat cosave record — carries the per-sender cooldown
        // table. Frozen — changing it orphans data.
        inline constexpr std::uint32_t kRecordTypeId = 'NBVS';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
        void OnRevert();
    } // namespace NPCVisitBeat_Persistence
} // namespace NarrativeEngine
