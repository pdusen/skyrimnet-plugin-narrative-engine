#pragma once

#include <IBeat.h>

#include <cstdint>

namespace SKSE
{
    class SerializationInterface;
}

// AmbushBeat — the Narrative Beat System's "raise tension" reach into the
// world: spawn a small group of leveled bandits near the player and let
// vanilla combat run its course. Backed by the `_ne_BanditAmbushQuest`
// quest in NarrativeEngine.esp.
//
// Lifecycle (four-state per-beat model, see PHASE_06 doc):
//   COMPOSE — validate params (already done in OnStart), fire the quest
//             start via EnsureQuestStarted. On success → RUNNING; on
//             failure → CLEANUP.
//   RUNNING — periodically poll the quest's IsCompleted() bit. When the
//             quest reaches its terminal stage (bandits dead / player
//             fled), transition to CLEANUP.
//   CLEANUP — Stop / Reset / SetEnabled(false) the quest, stamp the
//             per-beat cooldown, return to NOT_RUNNING.
namespace NarrativeEngine
{
    class AmbushBeat : public IBeat
    {
    public:
        std::string Name() const override;
        std::string Description() const override;
        BeatPolarity Polarity() const override;
        bool IsAvailable(const BeatContext& ctx) const override;
        void OnStart(const BeatContext& ctx, const nlohmann::json& parameters) override;
        TickResult Tick(TickMode mode, BeatState state) override;
        double RemainingCooldownGameHours() const override;
        void Abort() override;
    };

    namespace AmbushBeat_Persistence
    {
        // Per-beat cosave record. Frozen — changing it orphans data.
        inline constexpr std::uint32_t kRecordTypeId = 'NBAM';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
        void OnRevert();
    } // namespace AmbushBeat_Persistence
} // namespace NarrativeEngine
