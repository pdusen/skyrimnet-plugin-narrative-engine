#pragma once

#include <DecisionLog.h>
#include <Snapshot.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace SKSE
{
    class SerializationInterface;
}

// ActionDispatcher — orchestrates action selection, execution, and
// lifecycle tracking.
//
// State the dispatcher owns:
//   * actionInFlight       — name of the currently-running action ("" = none)
//   * actionStartedAt      — Unix-epoch seconds when the action was started
//   * lastActionCompletedAt — Unix-epoch seconds of the most recent completion
//   * recentlyFiredActions — small ring buffer of {name, completedAt} for
//                            the anti-repetition gate
//
// All except recentlyFiredActions persist across save/load via the 'NEAC'
// co-save record.
//
// Per-tick flow (Step 5 cut: no LLM call yet — just gate logging):
//   1. EvaluationPipeline calls ConsiderAction(snapshot, provisional) after
//      ParseDecision.
//   2. ConsiderAction walks the gates: in-flight, just-advanced, cooldown,
//      dwell time, candidate filter (IAction::IsAvailable + recency).
//   3. When all gates pass it logs "would consider action" (Step 5) — Step
//      6 wires the LLM call in here.
//
// Completion flow:
//   1. Papyrus (the quest) sends the _ne_ActionCompleted ModEvent with
//      strArg = the action name.
//   2. The dispatcher's sink (registered in Initialize) receives it on a
//      non-main thread, then marshals to the main thread.
//   3. On main: validate strArg matches actionInFlight, clear in-flight,
//      record completion time, push to recently-fired ring, push a fresh
//      dashboard state.
//
// Stale-lock recovery:
//   * OnTick runs from Tick's main-thread poll. If an action has been in
//     flight longer than iActionStaleLockTimeoutSeconds, the dispatcher
//     clears it with a logged warning.
namespace NarrativeEngine::ActionDispatcher
{
    // SKSE co-save record type ID. Frozen — changing it would orphan every
    // previously-saved ActionDispatcher payload.
    inline constexpr std::uint32_t kRecordTypeId = 'NEAC';

    // Wire the completion ModEvent sink. Call once at kDataLoaded after
    // SkyrimNetAPI / AsyncDispatch are initialized.
    void Initialize();

    // Per-evaluation hook called from EvaluationPipeline on the main
    // thread after ParseDecision produces a provisional record.
    //
    // The dispatcher TAKES OWNERSHIP of `snapshot` and `rec`. It walks the
    // gates and either:
    //   * Fires an action-select LLM call (when all gates pass and at
    //     least one candidate is available). The LLM callback eventually
    //     populates rec.actionSelected / actionParametersJSON /
    //     narrativeNote, calls EvaluationPipeline::ApplyDecision, and
    //     invokes `onFinalized` exactly once on the main thread.
    //   * Skips the LLM call (any gate blocks). Calls
    //     EvaluationPipeline::ApplyDecision(rec) and `onFinalized`
    //     immediately, still on the main thread.
    //
    // `onFinalized` runs after ApplyDecision. The caller uses it to release
    // any per-tick guards (e.g. EvaluationPipeline's g_inFlight flag).
    using FinalizedCallback = std::function<void()>;
    void ConsiderAction(Snapshot                      snapshot,
                        DecisionLog::DecisionRecord   rec,
                        FinalizedCallback             onFinalized);

    // Per-tick driver (main thread). Currently just runs the stale-lock
    // check. Cheap — bool compare + a time delta.
    void OnTick();

    // Quick query — main thread or otherwise. Useful for the dashboard.
    bool IsActionInFlight();

    // Snapshot of the in-flight state for dashboard rendering. nullopt when
    // no action is in flight.
    struct InFlightInfo
    {
        std::string name;
        double      startedAt = 0.0;
    };
    std::optional<InFlightInfo> GetInFlightInfo();

    // Public C++ entry point for marking an action as completed —
    // does the same work as the _ne_ActionCompleted ModEvent sink
    // (push to recently-fired ring, clear in-flight, stamp
    // lastActionCompletedAt, push a fresh dashboard state). Use this
    // for actions whose completion is best signaled from C++ rather
    // than via a Papyrus round trip (e.g. NPCLetterAction's LLM-
    // failure branch, which we'd otherwise have to bounce through
    // Papyrus just to land on the same handler).
    //
    // Main-thread only. Safe to call when the named action is the
    // current in-flight; logs a warning and no-ops if not.
    void CompleteAction(std::string_view actionName);

    // Force-dispatch an action via the debug UI. Runs the normal
    // action-select LLM pipeline but with a one-element candidate list
    // (the named action) and bypasses every cooldown / dwell /
    // repetition gate — the action's own IsAvailable() is skipped too.
    // The single-flight lock is still respected: refuses cleanly when
    // an action is already in-flight (log + return; the dashboard's
    // Dispatch button is disabled in that case anyway).
    //
    // The "rest of the pipeline behaves normally" after the LLM call
    // fires: response goes through the standard FinalizeWithLLMResponse
    // path, so validation, in-flight bookkeeping, and completion
    // handling all follow the shared code path.
    //
    // Main-thread only. Silently no-ops on unknown action names.
    void ForceDispatchAction(std::string_view actionName);

    // Co-save serialization callbacks. OnLoad receives the per-record
    // version and length advanced past the header by the central OnLoad
    // dispatcher in Plugin.cpp.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
}
