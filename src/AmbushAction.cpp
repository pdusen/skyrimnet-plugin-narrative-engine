#include <AmbushAction.h>

#include <LocationKeywords.h>
#include <Settings.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string_view>

namespace NarrativeEngine
{
    namespace
    {
        constexpr const char* kQuestEditorID = "_ne_BanditAmbushQuest";

        // Co-save record schema version. Bump only when the on-disk
        // layout changes; OnLoad falls back to clearing state on a
        // mismatch.
        constexpr std::uint32_t kRecordVersion = 1;

        // In-game hour stamp (RE::Calendar::GetHoursPassed at completion
        // time) of the most recent successful ambush. 0.0 means "never
        // completed since this character started." Guarded by g_mutex
        // because OnSave runs from SKSE's serialization callback while
        // the rest of the action runs from the main-thread tick driver.
        std::mutex g_mutex;
        double     g_lastCompletionGameHours = 0.0;

        // Current game-time in hours since the game's calendar epoch.
        // Safe to call on the main thread; nullopt if the calendar isn't
        // available yet (very early in plugin lifecycle).
        double CurrentGameHours()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            if (!calendar) {
                return 0.0;
            }
            return static_cast<double>(calendar->GetHoursPassed());
        }

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
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("AmbushAction::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        // Globally-disqualifying conditions (player in combat / dialogue /
        // scripted scene / DND cell) are gated by the ActionDispatcher; we
        // only need to check action-specific preconditions here.
        if (ctx.playerInInterior)   return blocked("playerInInterior");

        if (ctx.player) {
            if (LocationKeywords::IsSafe(ctx.player->GetCurrentLocation())) {
                return blocked("LocationKeywords::IsSafe");
            }
        }

        // Quest state is the source of truth for "is this ambush ready
        // to run?", with two caveats on which TESQuest flags are
        // actually trustworthy:
        //
        //   * IsCompleted() reads the kCompleted data-flag bit, which
        //     Papyrus CompleteQuest() sets and our cleanup path clears
        //     via Stop+Reset. Reliable — gate on it.
        //
        //   * IsRunning() reads the kEnabled bit, the SAME bit IsEnabled
        //     reads. It returns true for any enabled quest, including
        //     ones that have never been promoted (e.g. a "Start Game
        //     Enabled" quest at game load). Useless as a "really
        //     running" signal — do NOT gate on it.
        //
        //   * GetCurrentStageID() > 0 is the behavior-defined "really
        //     running" check: the quest's stage 0 fragment advances to
        //     stage 10 when the engine actually promotes the quest, and
        //     Reset() drops it back to 0 during cleanup. Combined with
        //     !IsCompleted(), this catches the in-flight window cleanly.
        //
        // No quest → can't ever run; treat as unavailable rather than
        // surfacing a Start-time failure.
        auto* quest = LookupAmbushQuest();
        if (!quest)                          return blocked("quest not found by EditorID");
        if (quest->IsCompleted())            return blocked("quest IsCompleted");
        if (quest->GetCurrentStageID() > 0)  return blocked("quest stage > 0 (in flight)");

        // Per-action in-game-hour cooldown. This sits on top of the
        // dispatcher's wall-clock global cooldown — the global gate
        // prevents back-to-back firing across all actions; this one
        // throttles the SAME action against in-fiction time so the
        // player doesn't get ambushed twice in the same afternoon.
        // Game-time also has the nice property that long real-world
        // pauses (alt-tabbing for hours) don't accidentally clear the
        // gate, and time spent sleeping / waiting / fast-traveling
        // does count toward unlocking it.
        const int cooldownHours = Settings::Get().ambushPerActionCooldownGameHours;
        if (cooldownHours > 0) {
            double lastCompletion = 0.0;
            {
                std::scoped_lock lock(g_mutex);
                lastCompletion = g_lastCompletionGameHours;
            }
            if (lastCompletion > 0.0) {
                const double elapsed = CurrentGameHours() - lastCompletion;
                if (elapsed < static_cast<double>(cooldownHours)) {
                    if (debug) {
                        logger::debug(
                            "AmbushAction::IsAvailable: blocked (per-action cooldown: "
                            "elapsed={:.2f}h < cooldown={}h, lastCompletion={:.2f}h current={:.2f}h)",
                            elapsed, cooldownHours, lastCompletion, CurrentGameHours());
                    }
                    return false;
                }
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

        // EnsureQuestStarted(result, startNow=true) is the C++ entry
        // point that matches console `startquest` semantics — full
        // engine promotion including the stage-0 fragment AND
        // Find-Matching-Reference alias evaluation. Other direct paths
        // we tried (SetEnabled+Start, SetEnabled+ResetAndUpdate, VM-
        // dispatched Start) flipped flags and sometimes advanced the
        // stage but never triggered the FMR pass, leaving alias slots
        // empty. See docs/engine-findings/starting-a-quest-from-cpp.md
        // for the full investigation.
        logger::info(
            "AmbushAction: starting '{}' (banditCount={} spawnDistance={})",
            kQuestEditorID, banditCount, spawnDistance);

        bool       engineResult = false;
        const bool callOk       = quest->EnsureQuestStarted(engineResult, true);
        if (!callOk || !engineResult) {
            logger::warn(
                "AmbushAction: EnsureQuestStarted reported failure (callOk={} engineResult={})",
                callOk, engineResult);
        }

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

        // Stamp the in-game hour of this successful completion so the
        // per-action cooldown (checked in IsAvailable) starts running.
        // Done AFTER the teardown so a teardown that throws / aborts
        // doesn't trip the cooldown gate on a non-completion.
        const double completionHours = CurrentGameHours();
        {
            std::scoped_lock lock(g_mutex);
            g_lastCompletionGameHours = completionHours;
        }
        logger::info(
            "AmbushAction: per-action cooldown stamp set to gameHours={:.2f}",
            completionHours);

        return true;
    }

    namespace AmbushAction_Persistence
    {
        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) return;
            if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
                logger::error("AmbushAction::OnSave: OpenRecord failed");
                return;
            }

            double stampCopy = 0.0;
            {
                std::scoped_lock lock(g_mutex);
                stampCopy = g_lastCompletionGameHours;
            }
            intfc->WriteRecordData(stampCopy);
        }

        void OnLoad(SKSE::SerializationInterface* intfc,
                    std::uint32_t                 version,
                    std::uint32_t                 length)
        {
            if (!intfc) return;
            if (version != kRecordVersion) {
                logger::warn(
                    "AmbushAction::OnLoad: unknown version {} (length={}); clearing cooldown stamp",
                    version, length);
                OnRevert();
                return;
            }

            double stampLoaded = 0.0;
            if (intfc->ReadRecordData(stampLoaded) != sizeof(stampLoaded)) {
                logger::error("AmbushAction::OnLoad: short read on completion stamp; clearing");
                OnRevert();
                return;
            }

            {
                std::scoped_lock lock(g_mutex);
                g_lastCompletionGameHours = stampLoaded;
            }
            logger::info(
                "AmbushAction::OnLoad: restored lastCompletionGameHours={:.2f}",
                stampLoaded);
        }

        void OnRevert()
        {
            std::scoped_lock lock(g_mutex);
            g_lastCompletionGameHours = 0.0;
        }
    }
}
