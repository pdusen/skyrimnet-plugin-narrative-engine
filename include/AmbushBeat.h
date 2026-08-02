#pragma once

#include <IBeat.h>

#include <SKSE/SKSE.h>

#include <RE/Skyrim.h>

#include <cstdint>

// AmbushBeat — the Narrative Beat System's hostile-encounter beat.
//
// A group of attackers, chosen by the Director from whatever groups the
// player's current game state makes plausible, materializes out of sight
// at a configurable distance, closes on the player under a travel
// package, and turns hostile at short range. See
// PHASE_11_AMBUSH_BEAT.md for the design.
//
// == What the previous implementation got wrong ==
//
// The old beat hung everything off a Find-Matching-Reference alias
// (`SpawnMarker`) gated on five stacked conditions, with all six
// attacker aliases set to "Create Reference to Object → At: SpawnMarker".
// When SpawnMarker failed to fill — which is most of the open
// wilderness, since it needed an approved marker type already placed
// nearby — every attacker alias failed with it, and the beat wedged in
// RUNNING waiting for OnDeath events from actors that were never
// created. This version searches geometry directly and force-fills the
// aliases from C++, so there is no fill rule left to fail.
//
// == Why aliases at all, then ==
//
// Spawned references still live in quest aliases rather than being
// tracked by FormID in the cosave, for three reasons: alias-held
// references are persistent and survive cell reset; the alias carries
// `_ne_AmbushApproach` in its PackageData so the engine instances the
// approach package per attacker with no extra work; and cleanup is a
// walk over the aliases rather than a list we maintained ourselves
// across saves.
//
// Aliases 1-8 are flagged Optional, so a 4-attacker ambush fills slots
// 0-3 and leaves the rest empty rather than failing the quest.
//
// == Lifecycle ==
//
//   COMPOSE — sub-state machine:
//               SelectingPoints — resolve group + count, find spawn points
//               StartingQuest   — EnsureQuestStarted (quest self-advances 0 → 10)
//               Spawning        — CreateReferenceAtLocation + VM-dispatch fills
//               VerifyingFill   — read the aliases back on a LATER tick
//               Arming          — aggression 0, EvaluatePackage, stage 10 → 20
//             Any failure → CLEANUP with a specific failure_reason and NO
//             cooldown stamp.
//   RUNNING — 5 s poll. Classifies each attacker alive / dead / gone,
//             drives the engage handoff at close range, and ends on
//             all-dead, outrun, or timeout.
//   CLEANUP — delete survivors (never corpses), stage 200, Stop, Reset,
//             stamp the cooldown only if COMPOSE actually succeeded.
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
        TickResult Tick(const PluginThread::Token& pt, TickMode mode, BeatState state) override;
        double RemainingCooldownGameHours() const override;
        void Abort(const MainThread::Token& mt) override;
    };

    namespace AmbushBeat_Init
    {
        // Resolve `_ne_AmbushQuest` and its nine reference aliases.
        // Called at kDataLoaded after Settings::Load and after
        // AmbushAttackerGroups::Load.
        void Initialize();
    } // namespace AmbushBeat_Init

    namespace AmbushBeat_Persistence
    {
        // Per-beat cosave record.
        //
        // Deliberately NOT the old beat's 'NBAM'. The new beat registers
        // under the same name the old one used ("ambush"), so
        // BeatSystem::OnLoad's "unknown beat → reset to idle" recovery
        // will no longer fire for it. A stale 'NBAM' payload from a
        // pre-removal save must therefore hit Plugin.cpp's default arm
        // and be skipped with a warning, rather than being misread as
        // this record's shape. 'NBAM' is permanently retired.
        inline constexpr std::uint32_t kRecordTypeId = 'NAMB';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
        void OnRevert();
    } // namespace AmbushBeat_Persistence
} // namespace NarrativeEngine
