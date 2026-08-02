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
// == Why the spawned references live in aliases ==
//
// Rather than being tracked by FormID in the cosave: alias-held
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
//               Start           — re-check the location
//               SelectingPoints — resolve group + count, find spawn points
//               StartingQuest   — retire the last encounter, EnsureQuestStarted
//                                 (quest self-advances 0 → 10)
//               Spawning        — PlaceObjectAtMe + VM-dispatch alias fills
//               VerifyingFill   — read the aliases back on a LATER tick
//               Settling        — let physics resolve, relocate anyone who
//                                 landed badly
//               Arming          — aggression 0, EvaluatePackage, start the
//                                 escort, stage 10 → 20
//             Any failure → CLEANUP with a specific failure_reason and NO
//             cooldown stamp.
//   RUNNING — the engage handoff at close range, escort checks for
//             attackers that aren't travelling, and a 5 s poll that
//             classifies each attacker alive / dead / gone and ends the
//             beat on all-dead, outrun, or timeout.
//   CLEANUP — delete survivors (never corpses), stage 200, stamp the
//             cooldown only if COMPOSE actually succeeded. The quest is
//             deliberately left running to hold the corpses in their
//             aliases; the next dispatch retires it.
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
        // Per-beat cosave record. 'NBAM' is retired and must never be
        // reused here: a payload under that id has a different shape,
        // and it needs to keep falling through to Plugin.cpp's default
        // arm to be skipped with a warning.
        inline constexpr std::uint32_t kRecordTypeId = 'NAMB';

        void OnSave(SKSE::SerializationInterface* intfc);
        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
        void OnRevert();
    } // namespace AmbushBeat_Persistence
} // namespace NarrativeEngine
