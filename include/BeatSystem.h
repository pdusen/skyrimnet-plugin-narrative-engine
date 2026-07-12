#pragma once

#include <DecisionLog.h>
#include <IBeat.h>
#include <Snapshot.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace SKSE
{
    class SerializationInterface;
}

// BeatSystem — the Narrative Beat System's master poll driver and top-
// level state owner.
//
// Runs a dedicated worker thread on a 250ms cadence (configurable via
// iBeatSystemPollIntervalMs). Each tick reads the three engine gates
// (paused / combat / dialogue), builds a TickMode, and dispatches based
// on the top-level state:
//   NO_BEAT_RUNNING — increment global cooldown counter under Normal mode
//   BEAT_RUNNING    — call the running beat's Tick(mode, state)
//
// The top-level state lives in the SKSE co-save record 'NBSY'. Step 4
// stands up the master poll and in-memory state only; Step 5 wires
// cosave; Step 6 wires the Director's beat-select handshake
// (ConsiderBeat / StartBeat).
//
// See docs/implementation/PHASE_06_BEAT_SYSTEM_REFACTOR.md.
namespace NarrativeEngine::BeatSystem
{
    // Top-level state's cosave record ID. Frozen — changing it orphans
    // saved payloads.
    inline constexpr std::uint32_t kRecordTypeId = 'NBSY';

    // Top-level machine state.
    enum class TopLevelState : std::uint8_t
    {
        NO_BEAT_RUNNING,
        BEAT_RUNNING,
    };

    // Start the master poll worker thread. Idempotent. Call once at
    // kDataLoaded after BeatRegistry::Initialize.
    void Initialize();

    // Signal the worker to exit and join. Idempotent. Safe from any
    // thread.
    void Shutdown();

    // Read-only accessors — thread-safe under an internal mutex. All
    // return snapshots; the underlying state may have changed by the
    // time the caller uses the value.
    TopLevelState GetTopLevelState();
    std::string   GetRunningBeatName();
    std::uint32_t GetGlobalCooldownMs();

    // In-flight query for the dashboard: name, wall-clock start time,
    // and current BeatState. Populated only when the top-level state
    // is BEAT_RUNNING.
    struct InFlightInfo
    {
        std::string name;
        double      startedAtRealSeconds = 0.0;
        BeatState   state                = BeatState::NOT_RUNNING;
    };
    // Returns std::nullopt when no beat is in flight.
    std::optional<InFlightInfo> GetInFlightInfo();

    // Per-evaluation hook called from EvaluationPipeline on the main
    // thread after ParseDecision produces a provisional record. Takes
    // ownership of snapshot and rec.
    //
    // Walks the top-level gates (already-running / cooldown / phase
    // dwell / global preconditions / candidate availability), fires
    // the beat-select LLM when the gates pass and there is at least
    // one candidate, and eventually calls onFinalized exactly once on
    // the main thread. When no candidates survive filtering, skips
    // cleanly with ApplyDecision + onFinalized.
    using FinalizedCallback = std::function<void()>;
    void ConsiderBeat(Snapshot                    snapshot,
                      DecisionLog::DecisionRecord rec,
                      FinalizedCallback           onFinalized);

    // Public entry point for handing dispatch control to a specific
    // registered beat. Main thread. Sets the top-level state to
    // BEAT_RUNNING with the given name, seeds the beat's OnStart from
    // the LLM-supplied parameters JSON, and stamps
    // BeatRegistry::MarkDispatched. Called from ConsiderBeat's LLM
    // response path and (indirectly, via ForceDispatchBeat) from the
    // dashboard's force-dispatch button.
    //
    // No-op when a beat is already running or when `name` isn't in the
    // registry. Logs and returns cleanly in both cases.
    void StartBeat(const std::string&    name,
                   const nlohmann::json& parameters);

    // Debug / dashboard entry point for force-dispatching a specific
    // beat. Bypasses every ConsiderBeat gate except the single-flight
    // lock and the global preconditions (combat / dialogue / scripted
    // scene / DND cell). Still runs the beat-select LLM against a
    // one-element candidate list so parameter validation flows through
    // the same code path as the normal dispatch.
    //
    // No-op when a beat is already running or when `name` isn't in the
    // registry. Logs and returns cleanly in both cases.
    void ForceDispatchBeat(std::string_view name);

    // Co-save handlers — implementations land in Step 5.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
}
