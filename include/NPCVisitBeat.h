#pragma once

#include <IBeat.h>

#include <SKSE/SKSE.h>

#include <RE/Skyrim.h>

#include <optional>

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
//               Stage 20 (Discuss)    — internal three-way substate cycle
//                                       (Discussing / OnHold / ReEngage) that
//                                       drives the speech sampler + conclusion
//                                       poll while free, pauses on any-party
//                                       combat (with combat-stuck watchdog),
//                                       and re-fires the resumption narration
//                                       on approach after combat clears — no
//                                       quest-stage change during the cycle
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
        TickResult Tick(const PluginThread::Token& pt, TickMode mode, BeatState state) override;
        void Abort(const MainThread::Token& mt) override;
    };

    namespace NPCVisitBeat_Init
    {
        // Resolve `_ne_VisitQuest`, `_ne_VisitSenderFaction`, the three
        // reference aliases (Sender, SpawnMarker, ReturnAnchor), and
        // wire the death sink. Called at kDataLoaded after
        // Settings::Load.
        void Initialize();
    } // namespace NPCVisitBeat_Init

    namespace NPCVisitBeat_Query
    {
        // The Discuss stage runs an internal three-way substate cycle
        // (Discussing / OnHold / ReEngage) that the dashboard's phase
        // display needs to distinguish. Not persisted; snapshot only.
        enum class DiscussSubPhase : std::uint8_t
        {
            Discussing,
            OnHold,
            ReEngage,
        };

        DiscussSubPhase GetDiscussSubPhase();
    } // namespace NPCVisitBeat_Query

    namespace NPCVisitBeat_Cooldowns
    {
        // Stamp the per-sender cooldown for `senderNpcFormID`. Called
        // when the Salutation → Discuss transition fires — the moment
        // we know the sender actually arrived and delivered their
        // opening line. Rolled-back / hard-aborted visits deliberately
        // do NOT stamp.
        void OnVisitCompleted(RE::FormID senderNpcFormID);

        // Stamp the per-sender memory watermark at Valediction entry
        // — the moment we know the beat has landed. Independent of
        // OnVisitCompleted's cooldown stamp; the two solve different
        // problems (cooldown throttles frequency; watermark filters
        // memory eligibility). Rolled-back / hard-aborted visits
        // deliberately do NOT stamp.
        void OnVisitReachedValediction(RE::FormID senderNpcFormID);

        // Filter helper — returns true if this sender is currently
        // within their per-sender cooldown window. Called by
        // VisitComposer during candidate viability filtering.
        bool IsSenderOnCooldown(RE::FormID senderNpcFormID);

        // Per-sender memory watermark accessor. Returns the absolute
        // game-hours of this sender's last completed visit (Valediction
        // entry), or nullopt if this sender has never visited through
        // the beat. Used by SenderCandidatePool as a hard filter that
        // drops memories predating the previous visit — prevents
        // re-selecting the same sender to visit about the same memory
        // a second time. Independent of IsSenderOnCooldown, which
        // decays; the watermark does not.
        std::optional<double> GetSenderMemoryWatermarkGameHours(RE::FormID senderNpcFormID);
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
