#include <AmbushAction.h>

#include <AlphaCanon.h>
#include <ConsoleCommand.h>
#include <LocationKeywords.h>
#include <Settings.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace NarrativeEngine
{
    namespace
    {
        constexpr const char* kQuestEditorID = "_ne_BanditAmbushQuest";

        RE::TESQuest* LookupAmbushQuest()
        {
            auto* form = RE::TESForm::LookupByEditorID(kQuestEditorID);
            if (!form) {
                return nullptr;
            }
            return form->As<RE::TESQuest>();
        }

        int ClampParameterInt(const nlohmann::json& parameters,
                              std::string_view      key,
                              int                   def,
                              int                   lo,
                              int                   hi)
        {
            int value = def;
            if (parameters.is_object()) {
                if (auto it = parameters.find(key); it != parameters.end() && it->is_number()) {
                    value = it->get<int>();
                }
            }
            return std::clamp(value, lo, hi);
        }
    }

    std::string AmbushAction::Name() const
    {
        return "ambush";
    }

    std::string AmbushAction::Description() const
    {
        return
            "A small group of leveled bandits (up to six) materializes at nearby "
            "world markers, jogs toward the player ignoring intervening NPCs, and "
            "engages in vanilla combat at close range. Best fit when the player is "
            "wandering open wilderness or a road with no obvious threat and the "
            "story has gone quiet — the ambush is high-visibility, clearly an "
            "intervention, and resolves in a single fight rather than escalating "
            "an existing situation. Not appropriate when the player is already in "
            "combat, in a settled area, or anywhere a fresh bandit attack would "
            "read as nonsensical (e.g. inside a city or inn).";
    }

    ActionPolarity AmbushAction::Polarity() const
    {
        return ActionPolarity::Raise;
    }

    bool AmbushAction::IsAvailable(const ActionContext& ctx) const
    {
        if (ctx.playerInInterior)   return false;
        if (ctx.playerInCombat)     return false;
        if (ctx.playerInDialogue)   return false;

        // AlphaCanon already covers scripted-scene and do-not-disturb cell
        // checks against live engine state — cheap to consult here even
        // though the dispatcher also gates on them at the snapshot level.
        if (AlphaCanon::IsInScriptedScene())    return false;
        if (AlphaCanon::IsInDoNotDisturbCell()) return false;

        if (ctx.player) {
            if (LocationKeywords::IsSafe(ctx.player->GetCurrentLocation())) {
                return false;
            }
        }

        return true;
    }

    StartResult AmbushAction::Start(const ActionContext&  ctx,
                                    const nlohmann::json& parameters)
    {
        // Re-validate availability — engine state may have shifted between
        // the dispatcher's filter pass and now (player walked into a town
        // boundary, hit a triggered scene, etc.).
        if (!IsAvailable(ctx)) {
            return {false, "preconditions failed at start time"};
        }

        const auto& cfg = Settings::Get();

        // The LLM may supply these; the quest's Papyrus design currently
        // ignores them (the six FMR-filled aliases are fixed). They're
        // clamped and recorded so Phase 04 can wire them through without
        // changing the parameter shape, and so the DecisionRecord captures
        // what the LLM intended.
        const int banditCount = ClampParameterInt(
            parameters, "bandit_count",
            cfg.ambushDefaultBanditCount,
            cfg.ambushMinBanditCount,
            cfg.ambushMaxBanditCount);
        const int spawnDistance = ClampParameterInt(
            parameters, "spawn_distance_units",
            cfg.ambushDefaultSpawnDistanceUnits,
            cfg.ambushMinSpawnDistanceUnits,
            cfg.ambushMaxSpawnDistanceUnits);

        auto* quest = LookupAmbushQuest();
        if (!quest) {
            return {false,
                    std::string("quest '") + kQuestEditorID +
                    "' not found by EditorID (ESP not loaded, or powerofthree's Tweaks missing?)"};
        }

        // TEMPORARY: dispatching via the console `startquest` command
        // rather than the direct C++ API.
        //
        // The direct path (commented out below) — SetEnabled(true) +
        // ResetAndUpdate() — successfully flips the quest's enabled /
        // running flags and runs the stage 0 fragment, but does NOT
        // trigger FMR alias evaluation, leaving the quest "running at
        // stage 10" with empty alias slots. The same quest, in the same
        // in-game state, starts cleanly (including FMR fill) when
        // `startquest _ne_BanditAmbushQuest` is typed at the console.
        //
        // Until we identify the C++ entry point that fully replicates
        // console behavior, we route through ConsoleCommand::Run, which
        // compiles and dispatches the command exactly as the console's
        // input handler does. This is a workaround, not the intended
        // long-term path.
        logger::info(
            "AmbushAction: starting '{}' via console command (banditCount={} spawnDistance={})",
            kQuestEditorID, banditCount, spawnDistance);

        // quest->SetEnabled(true);
        // quest->ResetAndUpdate();
        ConsoleCommand::Run(std::string("startquest ") + kQuestEditorID);

        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "ambush started: %d bandits at ~%du (params advisory)",
                      banditCount, spawnDistance);
        return {true, std::string(detail)};
    }

    bool AmbushAction::DetectAndRollbackFailedStart(const ActionContext& /*ctx*/,
                                                    double                secondsSinceStart)
    {
        // Grace period: the engine's promotion pass needs a frame or two
        // to run the stage fragment, evaluate FMR conditions on each
        // alias, and bind matching references. Two seconds is generously
        // larger than the actual window but tight enough that a stuck
        // quest doesn't waste a whole tick.
        constexpr double kGracePeriodSeconds = 2.0;
        if (secondsSinceStart < kGracePeriodSeconds) {
            return false;
        }

        auto* quest = LookupAmbushQuest();
        if (!quest) {
            // ESP unloaded mid-flight — nothing to roll back.
            return false;
        }

        // Success signal: every REQUIRED ReferenceAlias on the quest is
        // bound to a real reference. "Required" is the inverse of
        // Optional — the kOptional flag (BGSBaseAlias::FLAGS bit 1) is
        // explicitly set in CK on aliases that are allowed to remain
        // empty. Anything without it must fill for the quest to be
        // considered properly promoted.
        //
        // We restrict the check to BGSRefAlias because that's the only
        // alias type we know how to verify via GetReference(). Other
        // alias types (LocationAlias, etc.) are skipped — we don't
        // penalize success on aliases we can't introspect.
        //
        // Stage > 0 alone isn't sufficient: observed empirically that
        // ResetAndUpdate runs the stage fragment (so stage advances)
        // without triggering FMR evaluation, leaving the quest
        // "running at stage 10" but with empty alias slots and no
        // bandits in the world. Alias fill is the only check that
        // reliably correlates with the encounter actually existing.
        std::size_t requiredRefCount       = 0;
        std::size_t requiredRefFilledCount = 0;
        for (auto* alias : quest->aliases) {
            if (!alias) continue;
            if (alias->flags.any(RE::BGSBaseAlias::FLAGS::kOptional)) continue;
            auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(alias);
            if (!refAlias) continue;
            ++requiredRefCount;
            if (refAlias->GetReference() != nullptr) {
                ++requiredRefFilledCount;
            }
        }

        if (requiredRefCount == 0 || requiredRefFilledCount == requiredRefCount) {
            // Either no introspectable required aliases (success by
            // default — nothing to verify), or every one of them filled.
            // Let the action run to completion.
            return false;
        }

        logger::warn(
            "AmbushAction: only {}/{} required ref aliases filled after {:.1f}s (stage={}); tearing down for retry",
            requiredRefFilledCount, requiredRefCount,
            secondsSinceStart, quest->GetCurrentStageID());

        // Stop → Reset → SetEnabled(false) returns the quest fully to
        // its game-load baseline (stopped, disabled, stage 0). Stop
        // alone leaves the current stage advanced (since the fragment
        // already ran), which would skew any future restart attempt.
        // Reset here is safe — unlike the SetEnabled+Reset+Start
        // sequence we ruled out earlier (which poisoned alias slots
        // mid-promotion), this Reset happens on a fully-stopped quest
        // as a cleanup step before next-tick re-attempt.
        quest->Stop();
        quest->Reset();
        quest->SetEnabled(false);

        return true;
    }

    bool AmbushAction::DetectCompletion(const ActionContext& /*ctx*/,
                                        double /*secondsSinceStart*/)
    {
        auto* quest = LookupAmbushQuest();
        if (!quest) {
            // ESP unloaded mid-flight — treat as completion so the
            // dispatcher can clear in-flight; there's nothing left to
            // observe and re-firing would just fail at lookup.
            return true;
        }

        // The quest's stage 200 ("Complete Quest" marked stage) sets the
        // kCompleted bit on the quest's data flags. IsCompleted() reads
        // that bit. Note: a completed quest stays IsRunning() == true
        // until explicitly stopped — kRunning and kCompleted are
        // independent flags. That's why the dispatcher's existing
        // in-flight bookkeeping can't infer completion from IsRunning
        // alone and needs this poll.
        if (!quest->IsCompleted()) {
            return false;
        }

        logger::info(
            "AmbushAction: quest '{}' reached completed state (stage={}); cleaning up",
            kQuestEditorID, quest->GetCurrentStageID());

        // Same teardown shape as the rollback path: Stop returns the
        // quest's running flag to false, Reset clears stage / alias
        // state back to baseline, and SetEnabled(false) flips the
        // enabled flag off so a future Start sequence begins from the
        // same baseline the engine has at game-load.
        quest->Stop();
        quest->Reset();
        quest->SetEnabled(false);

        return true;
    }
}
