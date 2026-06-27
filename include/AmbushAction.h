#pragma once

#include <IAction.h>

#include <cstdint>

namespace SKSE
{
    class SerializationInterface;
}

// AmbushAction — the Director's "raise tension" reach into the world: spawn
// a small group of leveled bandits near the player and let vanilla combat
// run its course. Backed by the `_ne_BanditAmbushQuest` quest in
// NarrativeEngine.esp (CK + Papyrus authored in Step 7); this class is the
// thin C++ shim that the action toolbox dispatches against.
//
// Start() calls `RE::TESQuest::Start()` directly — the quest's
// Find-Matching-Reference aliases self-bind to nearby XMarkers and the
// per-alias Papyrus script (_ne_BanditAmbushQuest_SpawnedBandit) handles
// approach + combat handoff. The completion signal arrives via the shared
// `_ne_ActionCompleted` ModEvent that the quest's Papyrus sends when all
// bandits are dead — see ActionDispatcher's sink.
//
// Per-action state: the in-game hour stamp of the most recent successful
// completion. Used to enforce iAmbushPerActionCooldownGameHours on top of
// the dispatcher's global cooldown. Persists via the 'NEAB' co-save record.
namespace NarrativeEngine
{
    class AmbushAction : public IAction
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

    namespace AmbushAction_Persistence
    {
        // SKSE co-save record type ID for the per-action cooldown stamp.
        // Frozen — changing it would orphan previously-saved data.
        inline constexpr std::uint32_t kRecordTypeId = 'NEAB';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
        void OnRevert();
    }
}
